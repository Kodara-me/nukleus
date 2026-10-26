#include "WebService.h"
#include "ImageUpload.h"
#include "Multipart.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <iterator>
#include <optional>

namespace NukleusWeb
{
namespace
{
struct StaticFile
{
    const char *path;
    const char *resource;
    const char *mimeType;
};

constexpr StaticFile StaticFiles[] = {
    {"/", ":/ui/index.html", "text/html; charset=utf-8"},
    {"/index.html", ":/ui/index.html", "text/html; charset=utf-8"},
    {"/app.css", ":/ui/app.css", "text/css; charset=utf-8"},
    {"/app.js", ":/ui/app.js", "text/javascript; charset=utf-8"},
};

HttpServerOptions serverOptions(const WebConfig &config)
{
    HttpServerOptions options;
    options.lanAddress = config.listenAddress;
    options.port = config.port;
    options.hostName = config.hostName;
    options.idleTimeoutMs = config.idleTimeoutMs;
    return options;
}

constexpr int ModelRefreshTimeoutMs = 5000;

HttpResponse jsonResponse(const QJsonObject &object, int status = 200)
{
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    response.headers.emplaceBack("Cache-Control", "no-store");
    return response;
}

HttpResponse errorResponse(int status, const QString &message)
{
    return jsonResponse({{QStringLiteral("error"), message}}, status);
}

HttpResponse notFound()
{
    HttpResponse response;
    response.status = 404;
    response.body = "Not found.\n";
    return response;
}

enum class ActionKind { TextToImage, ImageToImage, Edit, ImageToVideo };

struct ActionDefinition
{
    ActionKind kind;
    const char *id;
    const char *title;
};

// The order of R1.
constexpr ActionDefinition Actions[] = {
    {ActionKind::TextToImage, "text-to-image", "Text to image"},
    {ActionKind::ImageToImage, "image-to-image", "Image to image"},
    {ActionKind::Edit, "edit", "Edit"},
    {ActionKind::ImageToVideo, "image-to-video", "Image to video"},
};

constexpr int MaximumWaitingJobs = 8;     // R35, D24
constexpr int MaximumReferenceImages = 7; // R6

bool recommendedFlag(const Nukleus::ModelInfo &model, const char *name)
{
    return model.recommendedOptions.value(QLatin1String(name)).toBool(true);
}

// The models an action offers (R1): `canGenerate` for Text to image and Image to image, `canEdit` for
// Edit, both read from the recommended options and true when absent; Animation models with all their
// files for Image to video.
bool offers(ActionKind kind, const Nukleus::ModelInfo &model)
{
    if (kind == ActionKind::ImageToVideo)
        return model.category == Nukleus::ModelCategory::Animation && model.missingFiles.isEmpty();
    if (model.category != Nukleus::ModelCategory::ImageGeneration) return false;
    return recommendedFlag(model, kind == ActionKind::Edit ? "canEdit" : "canGenerate");
}

Nukleus::ModelCategory categoryOf(ActionKind kind)
{
    return kind == ActionKind::ImageToVideo ? Nukleus::ModelCategory::Animation : Nukleus::ModelCategory::ImageGeneration;
}

bool supported(const Nukleus::ServiceClient *client, ActionKind kind)
{
    switch (kind) {
    case ActionKind::TextToImage:
    case ActionKind::ImageToImage: return client->supports(Nukleus::ImageOperation::Generate);
    case ActionKind::Edit: return client->supports(Nukleus::ImageOperation::Edit);
    case ActionKind::ImageToVideo: return client->canAnimate();
    }
    return false;
}

QJsonObject modelJson(const Nukleus::ModelInfo &model)
{
    QJsonObject options;
    for (const QString &key : {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("steps"), QStringLiteral("cfgScale")})
        if (model.recommendedOptions.value(key).isDouble()) options.insert(key, model.recommendedOptions.value(key));
    return {{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.displayLabel()}, {QStringLiteral("recommendedOptions"), options}};
}

HttpResponse busyResponse()
{
    return errorResponse(503, QStringLiteral("The service is busy. Try again in a moment."));
}

QByteArray extensionFor(const QByteArray &mimeType)
{
    if (mimeType == "image/png") return "png";
    if (mimeType == "image/jpeg") return "jpg";
    if (mimeType == "image/webp") return "webp";
    if (mimeType == "video/mp4") return "mp4";
    if (mimeType == "video/webm") return "webm";
    return "bin";
}

// Reads a numeric field: absent or blank gives `fallback`, a malformed value fails.
template <typename T>
bool readNumber(const QHash<QString, QString> &fields, const QString &name, T fallback, T *value)
{
    const QString text = fields.value(name).trimmed();
    if (text.isEmpty()) { *value = fallback; return true; }
    bool ok = false;
    if constexpr (std::is_same_v<T, int>) *value = text.toInt(&ok);
    else *value = text.toDouble(&ok);
    return ok;
}

void addSecurityHeaders(HttpResponse &response)
{
    response.headers.emplaceBack("X-Content-Type-Options", "nosniff");
    response.headers.emplaceBack("Referrer-Policy", "no-referrer");
    response.headers.emplaceBack("X-Frame-Options", "DENY");
    response.headers.emplaceBack("Content-Security-Policy",
        "default-src 'self'; img-src 'self' blob:; media-src 'self' blob:; frame-ancestors 'none'");
}
}

WebService::WebService(const WebConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_link(config, this)
    , m_server(serverOptions(config), [this](const HttpRequest &request, HttpResponder respond) { handle(request, std::move(respond)); }, this)
{
    m_modelTimeout.setSingleShot(true);
    m_modelTimeout.setInterval(ModelRefreshTimeoutMs);
    connect(&m_modelTimeout, &QTimer::timeout, this, &WebService::finishModelRefresh);
    connect(&m_link, &BackendLink::stateChanged, this, &WebService::finishModelRefresh);
    Nukleus::ServiceClient *client = m_link.client();
    connect(client, &Nukleus::ServiceClient::modelsChanged, this, &WebService::finishModelRefresh);
    connect(client, &Nukleus::ServiceClient::jobStateChanged, this,
            [this](const QString &id, Nukleus::JobState state, int position) { onJobStateChanged(id, state, position); });
    connect(client, &Nukleus::ServiceClient::generationProgressChanged, this,
            [this](const QString &id, const Nukleus::GenerationProgress &progress) { onProgress(id, progress); });
    connect(client, &Nukleus::ServiceClient::imageJobFinished, this,
            [this](const Nukleus::ImageJobResult &result) { onImageJobFinished(result); });
    connect(client, &Nukleus::ServiceClient::animationFinished, this,
            [this](const Nukleus::AnimationResult &result) { onAnimationFinished(result); });
}

bool WebService::start(QString *error)
{
    if (!m_server.listen(error)) return false;
    QStringList addresses;
    for (const QHostAddress &address : m_server.listenAddresses()) addresses << address.toString();
    qInfo().noquote() << QStringLiteral("[Nukleus Web] Listening on %1 port %2.").arg(addresses.join(QStringLiteral(", "))).arg(m_server.port());
    m_link.start();
    return true;
}

void WebService::withFreshModels(std::function<void()> then)
{
    if (m_link.state() != BackendLink::State::Available) { then(); return; }
    m_modelWaiters.append(std::move(then));
    if (m_modelWaiters.size() > 1) return; // a request is already in flight: wait for its result
    m_modelTimeout.start();
    m_link.client()->requestModels();
}

void WebService::finishModelRefresh()
{
    if (m_modelWaiters.isEmpty()) return;
    m_modelTimeout.stop();
    const auto waiters = std::exchange(m_modelWaiters, {});
    for (const auto &waiter : waiters) waiter();
}

QJsonObject WebService::actionJson(int index) const
{
    const ActionDefinition &definition = Actions[index];
    QJsonObject action{{QStringLiteral("id"), QString::fromLatin1(definition.id)}, {QStringLiteral("title"), QString::fromLatin1(definition.title)}};
    QJsonArray models;
    for (const Nukleus::ModelInfo &model : m_link.client()->models(categoryOf(definition.kind)))
        if (offers(definition.kind, model)) models.append(modelJson(model));
    QString unavailable;
    if (!supported(m_link.client(), definition.kind)) unavailable = QStringLiteral("unsupported-operation");
    else if (models.isEmpty()) unavailable = QStringLiteral("no-model");
    action.insert(QStringLiteral("available"), unavailable.isEmpty());
    action.insert(QStringLiteral("unavailable"), unavailable.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(unavailable));
    action.insert(QStringLiteral("models"), unavailable.isEmpty() ? models : QJsonArray());
    return action;
}

void WebService::respondStatus(HttpResponder respond)
{
    withFreshModels([this, respond = std::move(respond)] {
        QString state = QStringLiteral("connecting");
        if (m_link.state() == BackendLink::State::Available) state = QStringLiteral("available");
        else if (m_link.state() == BackendLink::State::Unavailable) state = QStringLiteral("unavailable");
        QJsonObject object{{QStringLiteral("state"), state}};
        object.insert(QStringLiteral("reason"), m_link.reason().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m_link.reason()));
        QJsonArray actions;
        if (m_link.state() == BackendLink::State::Available)
            for (int i = 0; i < int(std::size(Actions)); ++i) actions.append(actionJson(i));
        object.insert(QStringLiteral("actions"), actions);
        object.insert(QStringLiteral("defaults"), QJsonObject{{QStringLiteral("width"), 1024}, {QStringLiteral("height"), 1024},
            {QStringLiteral("steps"), 28}, {QStringLiteral("cfgScale"), 7}, {QStringLiteral("seed"), -1}});
        object.insert(QStringLiteral("limits"), QJsonObject{{QStringLiteral("imageBytes"), double(MaximumImageBytes)},
            {QStringLiteral("jobImageBytes"), double(MaximumJobImageBytes)}, {QStringLiteral("references"), MaximumReferenceImages}});
        HttpResponse response = jsonResponse(object);
        addSecurityHeaders(response);
        respond(std::move(response));
    });
}

HttpResponse WebService::submitJob(const HttpRequest &request)
{
    if (m_link.state() != BackendLink::State::Available) return errorResponse(502, QStringLiteral("Nukleus is unavailable."));
    MultipartForm form;
    if (!parseMultipart(request.headers.value("content-type"), request.body, &form)) return errorResponse(400, QStringLiteral("The request is not a valid form."));
    // R13: the action names only image.generate, image.edit and image.animate jobs.
    const QString actionId = form.fields.value(QStringLiteral("action"));
    const ActionDefinition *definition = nullptr;
    for (const ActionDefinition &candidate : Actions)
        if (actionId == QLatin1String(candidate.id)) definition = &candidate;
    if (!definition) return errorResponse(400, QStringLiteral("Unsupported action."));
    const ActionKind kind = definition->kind;
    if (!supported(m_link.client(), kind)) return errorResponse(400, QStringLiteral("This Nukleus service does not offer this action."));
    if (m_waiting.size() >= MaximumWaitingJobs) return busyResponse(); // R35

    const QString modelId = form.fields.value(QStringLiteral("modelId"));
    std::optional<Nukleus::ModelInfo> chosen;
    for (const Nukleus::ModelInfo &model : m_link.client()->models(categoryOf(kind)))
        if (model.id == modelId && offers(kind, model)) chosen = model;
    if (!chosen) return errorResponse(400, QStringLiteral("Choose a model that can run this action."));

    // The images of the action: its form fields, whether each is required, and how many it takes (R5-R7).
    struct Input { const char *field; const char *role; bool required; int maximum; };
    QList<Input> inputs;
    switch (kind) {
    case ActionKind::TextToImage: break;
    case ActionKind::ImageToImage: inputs = {{"image", "source image", true, 1}}; break;
    case ActionKind::Edit: inputs = {{"source", "source image", true, 1}, {"reference", "reference image", false, MaximumReferenceImages}}; break;
    case ActionKind::ImageToVideo: inputs = {{"startImage", "start image", true, 1}, {"endImage", "end image", false, 1}}; break;
    }
    // A file input left empty reaches the server as an empty part: it is no image.
    QList<MultipartFile> files;
    for (const MultipartFile &file : form.files)
        if (!(file.fileName.isEmpty() && file.data.isEmpty())) files.append(file);
    qint64 total = 0;
    for (const MultipartFile &file : files) {
        const auto input = std::find_if(inputs.cbegin(), inputs.cend(), [&](const Input &candidate) { return file.name == QLatin1String(candidate.field); });
        if (input == inputs.cend()) return errorResponse(400, QStringLiteral("This action takes no image named \"%1\".").arg(file.name));
        total += file.data.size();
    }
    QList<QList<const MultipartFile *>> byInput;
    for (const Input &input : inputs) {
        QList<const MultipartFile *> chosenFiles;
        for (const MultipartFile &file : files)
            if (file.name == QLatin1String(input.field)) chosenFiles.append(&file);
        if (chosenFiles.isEmpty() && input.required) return errorResponse(400, QStringLiteral("Choose a %1.").arg(QLatin1String(input.role)));
        if (chosenFiles.size() > input.maximum) return errorResponse(400, QStringLiteral("Too many images for the %1.").arg(QLatin1String(input.role)));
        byInput.append(chosenFiles);
    }
    // R8: size limits, before any image is decoded.
    const auto labelOf = [](const MultipartFile &file, const Input &input) {
        return file.fileName.isEmpty() ? QString::fromLatin1(input.role) : file.fileName;
    };
    for (int i = 0; i < inputs.size(); ++i)
        for (const MultipartFile *file : byInput.at(i))
            if (file->data.size() > MaximumImageBytes)
                return errorResponse(413, QStringLiteral("The image \"%1\" is larger than 32 MiB.").arg(labelOf(*file, inputs.at(i))));
    if (total > MaximumJobImageBytes) return errorResponse(413, QStringLiteral("The images of one job are limited to 96 MiB in all."));

    // R36: the budget of an Edit image is the model's recommended width x height, 1024 x 1024 without one.
    const QJsonObject recommended = chosen->recommendedOptions;
    const qint64 budget = kind == ActionKind::Edit
        ? qint64(recommended.value(QStringLiteral("width")).toInt(1024)) * recommended.value(QStringLiteral("height")).toInt(1024) : 0;
    // R8, R36, R46: every image is decoded, so that one that cannot be read refuses the job, and an Edit image over the budget is scaled.
    QList<QList<PreparedImage>> images;
    for (int i = 0; i < inputs.size(); ++i) {
        QList<PreparedImage> prepared;
        for (const MultipartFile *file : byInput.at(i)) {
            PreparedImage image;
            QString message;
            if (!prepareImage(file->data, labelOf(*file, inputs.at(i)), budget, &image, &message)) return errorResponse(400, message);
            prepared.append(image);
        }
        images.append(prepared);
    }

    const auto number = [&](const char *name, auto fallback, auto *value) { return readNumber(form.fields, QLatin1String(name), fallback, value); };
    const QString prompt = form.fields.value(QStringLiteral("prompt"));
    const QString negativePrompt = form.fields.value(QStringLiteral("negativePrompt"));
    Sender send;
    Nukleus::ServiceClient *client = m_link.client();
    if (kind == ActionKind::ImageToVideo) {
        // R7: only the prompt, negative prompt, seed and images; mp4 when Nukleus can make it.
        Nukleus::AnimationRequest animation;
        animation.modelId = modelId;
        animation.prompt = prompt;
        animation.negativePrompt = negativePrompt;
        if (prompt.trimmed().isEmpty()) return errorResponse(400, QStringLiteral("Enter a prompt."));
        if (!number("seed", -1, &animation.seed)) return errorResponse(400, QStringLiteral("The seed must be a number."));
        animation.startImage = images.at(0).first().data;
        if (!images.at(1).isEmpty()) animation.endImage = images.at(1).first().data;
        if (client->canAnimateMp4()) animation.parameters.insert(QStringLiteral("container"), QStringLiteral("mp4"));
        send = [client, animation] { return client->submitAnimation(animation); };
    } else {
        Nukleus::ImageGenerationRequest generation;
        generation.modelId = modelId;
        generation.prompt = prompt;
        generation.negativePrompt = negativePrompt;
        bool valid = number("steps", recommended.value(QStringLiteral("steps")).toInt(28), &generation.steps)
            && number("cfgScale", recommended.value(QStringLiteral("cfgScale")).toDouble(7.0), &generation.cfgScale)
            && number("seed", -1, &generation.seed);
        if (kind != ActionKind::Edit)
            valid = valid && number("width", recommended.value(QStringLiteral("width")).toInt(1024), &generation.width)
                && number("height", recommended.value(QStringLiteral("height")).toInt(1024), &generation.height);
        if (!valid) return errorResponse(400, QStringLiteral("Width, height, steps, CFG scale and seed must be numbers."));
        if (kind == ActionKind::ImageToImage) {
            if (!number("strength", 0.55, &generation.baseImageStrength) || generation.baseImageStrength < 0 || generation.baseImageStrength > 1)
                return errorResponse(400, QStringLiteral("The source image strength must be a number from 0 to 1."));
            generation.baseImageData = images.at(0).first().data;
        }
        if (kind == ActionKind::Edit) {
            if (prompt.trimmed().isEmpty()) return errorResponse(400, QStringLiteral("Enter an instruction for the edit."));
            // R6: the source first, then the references; the job has the size of the source as sent.
            generation.width = images.at(0).first().size.width();
            generation.height = images.at(0).first().size.height();
            generation.editImages.append(images.at(0).first().data);
            for (const PreparedImage &reference : images.at(1)) generation.editImages.append(reference.data);
            send = [client, generation] { return client->submitEdit(generation); };
        } else {
            send = [client, generation] { return client->submitGeneration(generation); };
        }
    }
    const QString id = m_jobs.add().id;
    enqueue(id, std::move(send));
    return jsonResponse({{QStringLiteral("id"), id}}, 200);
}

void WebService::enqueue(const QString &jobId, Sender send)
{
    m_waiting.append({jobId, std::move(send)});
    sendNext();
}

// Sends the next waiting job, only when no job's images are being sent: nukleusd refuses a submission
// on a connection with an upload pending (R35).
void WebService::sendNext()
{
    while (!m_sending && !m_waiting.isEmpty()) {
        const Waiting next = m_waiting.takeFirst();
        WebJob *job = m_jobs.find(next.jobId);
        if (!job) continue; // no longer held (R44): nothing to show for it
        const QString sdkId = next.send();
        if (sdkId.isEmpty()) {
            job->state = WebJob::State::Failed;
            job->error = m_link.state() == BackendLink::State::Available ? QStringLiteral("Nukleus cannot run this request.")
                                                                         : QStringLiteral("Nukleus is unavailable.");
            continue;
        }
        job->sdkId = sdkId;
        m_sendingSdkId = sdkId;
        m_sending = true;
    }
}

void WebService::releaseSending(const QString &sdkId)
{
    if (!m_sending || sdkId != m_sendingSdkId) return;
    m_sending = false;
    m_sendingSdkId.clear();
    sendNext();
}

HttpResponse WebService::jobResponse(const QString &id)
{
    WebJob *job = m_jobs.find(id);
    if (!job) return errorResponse(404, QStringLiteral("This job is no longer available."));
    static const char *const states[] = {"queued", "running", "finished", "failed"};
    QJsonObject object{{QStringLiteral("id"), job->id}, {QStringLiteral("state"), QString::fromLatin1(states[int(job->state)])}};
    object.insert(QStringLiteral("queuePosition"), job->queuePosition >= 0 ? QJsonValue(job->queuePosition) : QJsonValue(QJsonValue::Null));
    object.insert(QStringLiteral("progress"), job->progress >= 0 ? QJsonValue(job->progress) : QJsonValue(QJsonValue::Null));
    object.insert(QStringLiteral("error"), job->error.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(job->error));
    if (job->state == WebJob::State::Finished) object.insert(QStringLiteral("mimeType"), QString::fromLatin1(job->mimeType));
    return jsonResponse(object);
}

HttpResponse WebService::resultResponse(const QString &id)
{
    WebJob *job = m_jobs.find(id);
    if (!job) return errorResponse(404, QStringLiteral("This job is no longer available."));
    if (job->state != WebJob::State::Finished) return errorResponse(409, QStringLiteral("This job has no result."));
    HttpResponse response;
    response.contentType = job->mimeType;
    response.body = job->result;
    response.headers.emplaceBack("Cache-Control", "no-store");
    response.headers.emplaceBack("Content-Disposition", "attachment; filename=\"nukleus-" + job->id.left(8).toLatin1() + '.' + extensionFor(job->mimeType) + '"');
    return response;
}

void WebService::onJobStateChanged(const QString &sdkId, Nukleus::JobState state, int queuePosition)
{
    // Nukleus has the job's images once it reports anything but an upload.
    if (state != Nukleus::JobState::Uploading) releaseSending(sdkId);
    WebJob *job = m_jobs.findBySdkId(sdkId);
    if (!job || job->state == WebJob::State::Finished || job->state == WebJob::State::Failed) return;
    if (state == Nukleus::JobState::Queued) {
        job->state = WebJob::State::Queued;
        if (queuePosition >= 0) job->queuePosition = queuePosition;
    } else if (state == Nukleus::JobState::Running) {
        job->state = WebJob::State::Running;
        job->queuePosition = -1;
        if (job->progress < 0) job->progress = 0;
    }
}

void WebService::onProgress(const QString &sdkId, const Nukleus::GenerationProgress &progress)
{
    WebJob *job = m_jobs.findBySdkId(sdkId);
    if (!job || job->state == WebJob::State::Finished || job->state == WebJob::State::Failed) return;
    job->state = WebJob::State::Running;
    job->queuePosition = -1;
    job->progress = qBound(0, progress.percentage, 100);
}

void WebService::onImageJobFinished(const Nukleus::ImageJobResult &result)
{
    finishJob(result.jobId, result.succeeded, result.imageData,
              result.mimeType.isEmpty() ? QByteArray("image/png") : result.mimeType.toLatin1(), result.message);
}

void WebService::onAnimationFinished(const Nukleus::AnimationResult &result)
{
    finishJob(result.jobId, result.succeeded, result.videoData,
              result.mimeType.isEmpty() ? QByteArray("video/webm") : result.mimeType.toLatin1(), result.message);
}

void WebService::finishJob(const QString &sdkId, bool succeeded, const QByteArray &data, const QByteArray &mimeType, const QString &message)
{
    if (WebJob *job = m_jobs.findBySdkId(sdkId)) { // none when no longer held (R44)
        if (succeeded) {
            job->state = WebJob::State::Finished;
            job->progress = 100;
            job->result = data;
            job->mimeType = mimeType;
        } else {
            job->state = WebJob::State::Failed;
            job->error = message.isEmpty() ? QStringLiteral("The job failed.") : message;
        }
        job->queuePosition = -1;
    }
    releaseSending(sdkId);
}

void WebService::handle(const HttpRequest &request, HttpResponder respond)
{
    if (request.method == "GET" && request.path == "/api/status") {
        respondStatus(std::move(respond));
        return;
    }
    HttpResponse response;
    const QString path = QString::fromLatin1(request.path);
    static const QString jobsPrefix = QStringLiteral("/api/jobs/");
    if (request.method == "POST" && path == QLatin1String("/api/jobs")) {
        response = submitJob(request);
    } else if (request.method == "GET" && path.startsWith(jobsPrefix)) {
        const QString rest = path.mid(jobsPrefix.size());
        if (rest.endsWith(QLatin1String("/result"))) response = resultResponse(rest.chopped(7));
        else response = jobResponse(rest);
    } else if (request.method == "GET") {
        bool found = false;
        for (const StaticFile &file : StaticFiles) {
            if (request.path != file.path) continue;
            QFile resource(QString::fromLatin1(file.resource));
            if (!resource.open(QIODevice::ReadOnly)) break;
            response.contentType = file.mimeType;
            response.body = resource.readAll();
            response.headers.emplaceBack("Cache-Control", "no-cache");
            found = true;
            break;
        }
        if (!found) response = notFound();
    } else {
        response = notFound();
    }
    addSecurityHeaders(response);
    respond(std::move(response));
}
}
