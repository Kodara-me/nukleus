#include "WebService.h"
#include "Multipart.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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

// A model offers Text to image unless its recommended options say canGenerate is false (R1).
bool canGenerate(const Nukleus::ModelInfo &model)
{
    return model.category == Nukleus::ModelCategory::ImageGeneration
        && model.recommendedOptions.value(QStringLiteral("canGenerate")).toBool(true);
}

QJsonObject modelJson(const Nukleus::ModelInfo &model)
{
    QJsonObject options;
    for (const QString &key : {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("steps"), QStringLiteral("cfgScale")})
        if (model.recommendedOptions.value(key).isDouble()) options.insert(key, model.recommendedOptions.value(key));
    return {{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.displayLabel()}, {QStringLiteral("recommendedOptions"), options}};
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

QJsonObject WebService::textToImageAction() const
{
    QJsonObject action{{QStringLiteral("id"), QStringLiteral("text-to-image")}, {QStringLiteral("title"), QStringLiteral("Text to image")}};
    QJsonArray models;
    for (const Nukleus::ModelInfo &model : m_link.client()->models(Nukleus::ModelCategory::ImageGeneration))
        if (canGenerate(model)) models.append(modelJson(model));
    QString unavailable;
    if (!m_link.client()->supports(Nukleus::ImageOperation::Generate)) unavailable = QStringLiteral("unsupported-operation");
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
        if (m_link.state() == BackendLink::State::Available) actions.append(textToImageAction());
        object.insert(QStringLiteral("actions"), actions);
        object.insert(QStringLiteral("defaults"), QJsonObject{{QStringLiteral("width"), 1024}, {QStringLiteral("height"), 1024},
            {QStringLiteral("steps"), 28}, {QStringLiteral("cfgScale"), 7}, {QStringLiteral("seed"), -1}});
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
    // R13: only image.generate is submitted; the other actions come with a later task.
    if (form.fields.value(QStringLiteral("action")) != QLatin1String("text-to-image")) return errorResponse(400, QStringLiteral("Unsupported action."));
    const QString modelId = form.fields.value(QStringLiteral("modelId"));
    const Nukleus::ModelInfo *chosen = nullptr;
    const QList<Nukleus::ModelInfo> models = m_link.client()->models(Nukleus::ModelCategory::ImageGeneration);
    for (const Nukleus::ModelInfo &model : models)
        if (model.id == modelId && canGenerate(model)) chosen = &model;
    if (!chosen) return errorResponse(400, QStringLiteral("Choose a model that can generate images."));

    // Fields left out take the model's recommended options, else the Nukleus defaults (R4).
    const QJsonObject recommended = chosen->recommendedOptions;
    Nukleus::ImageGenerationRequest generation;
    generation.modelId = modelId;
    generation.prompt = form.fields.value(QStringLiteral("prompt"));
    generation.negativePrompt = form.fields.value(QStringLiteral("negativePrompt"));
    const bool valid =
        readNumber(form.fields, QStringLiteral("width"), recommended.value(QStringLiteral("width")).toInt(1024), &generation.width)
        && readNumber(form.fields, QStringLiteral("height"), recommended.value(QStringLiteral("height")).toInt(1024), &generation.height)
        && readNumber(form.fields, QStringLiteral("steps"), recommended.value(QStringLiteral("steps")).toInt(28), &generation.steps)
        && readNumber(form.fields, QStringLiteral("cfgScale"), recommended.value(QStringLiteral("cfgScale")).toDouble(7.0), &generation.cfgScale)
        && readNumber(form.fields, QStringLiteral("seed"), -1, &generation.seed);
    if (!valid) return errorResponse(400, QStringLiteral("Width, height, steps, CFG scale and seed must be numbers."));
    const QString sdkId = m_link.client()->submitGeneration(generation);
    if (sdkId.isEmpty()) return errorResponse(400, QStringLiteral("Nukleus cannot run this request."));
    const WebJob &job = m_jobs.add(sdkId);
    return jsonResponse({{QStringLiteral("id"), job.id}}, 200);
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
    WebJob *job = m_jobs.findBySdkId(result.jobId);
    if (!job) return; // no longer held (R44)
    if (result.succeeded) {
        job->state = WebJob::State::Finished;
        job->progress = 100;
        job->result = result.imageData;
        job->mimeType = result.mimeType.isEmpty() ? QByteArray("image/png") : result.mimeType.toLatin1();
    } else {
        job->state = WebJob::State::Failed;
        job->error = result.message.isEmpty() ? QStringLiteral("The job failed.") : result.message;
    }
    job->queuePosition = -1;
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
