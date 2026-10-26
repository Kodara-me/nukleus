(function () {
  "use strict";

  var STATUS_POLL_MS = 3000;
  var JOB_POLL_MS = 1000;
  var STORAGE_KEY = "nukleus.jobs";
  var MAX_STORED_JOBS = 50;
  var REASONS = {
    "unreachable": "The Nukleus service cannot be reached. Check that it is running and that this service points at the right host.",
    "authentication-refused": "The Nukleus service refused this service's credentials. Check the backend token.",
    "incompatible-protocol": "The Nukleus service speaks a protocol version this service does not support. Update both to the same release."
  };
  var UNAVAILABLE = {
    "no-model": "No installed model can run this action. Install one with the Nukleus manager.",
    "unsupported-operation": "This Nukleus service does not offer this action."
  };

  var panel = document.getElementById("status");
  var title = document.getElementById("status-title");
  var detail = document.getElementById("status-detail");
  var retry = document.getElementById("retry");
  var actionSection = document.getElementById("action");
  var unavailable = document.getElementById("action-unavailable");
  var form = document.getElementById("form");
  var modelSelect = document.getElementById("model");
  var formError = document.getElementById("form-error");
  var submit = document.getElementById("submit");
  var jobsSection = document.getElementById("jobs-section");
  var jobsList = document.getElementById("jobs");
  var FIELDS = ["width", "height", "steps", "cfgScale", "seed"];

  var statusTimer = null;
  var models = [];
  var modelsKey = "";
  var defaults = { width: 1024, height: 1024, steps: 28, cfgScale: 7, seed: -1 };
  var jobs = loadJobs();
  var cards = {};

  // The job list lives in localStorage (R11); a browser without it still works for this page load.
  function loadJobs() {
    try {
      var stored = JSON.parse(localStorage.getItem(STORAGE_KEY) || "[]");
      return Array.isArray(stored) ? stored.filter(function (job) { return job && typeof job.id === "string"; }) : [];
    } catch (e) {
      return [];
    }
  }

  function saveJobs() {
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(jobs.slice(0, MAX_STORED_JOBS))); } catch (e) { /* kept in memory only */ }
  }

  function show(state, text, extra) {
    panel.setAttribute("data-state", state);
    title.textContent = text;
    detail.textContent = extra || "";
    retry.hidden = state !== "unavailable";
    panel.hidden = state === "available";
  }

  function prefill(model) {
    var options = (model && model.recommendedOptions) || {};
    FIELDS.forEach(function (name) {
      form.elements[name].value = options[name] !== undefined ? options[name] : defaults[name];
    });
  }

  function selectedModel() {
    for (var i = 0; i < models.length; i++) if (models[i].id === modelSelect.value) return models[i];
    return null;
  }

  function renderAction(action) {
    var list = action.models || [];
    var key = JSON.stringify(list);
    actionSection.hidden = false;
    unavailable.hidden = action.available;
    form.hidden = !action.available;
    if (!action.available) {
      unavailable.textContent = UNAVAILABLE[action.unavailable] || "This action is unavailable.";
      return;
    }
    if (key === modelsKey) return; // the lists did not change: keep what the user typed
    var previous = modelSelect.value;
    var first = modelsKey === "";
    models = list;
    modelsKey = key;
    modelSelect.textContent = "";
    list.forEach(function (model) {
      var option = document.createElement("option");
      option.value = model.id;
      option.textContent = model.name;
      modelSelect.appendChild(option);
    });
    var kept = list.some(function (model) { return model.id === previous; });
    if (kept) modelSelect.value = previous;
    if (first || !kept) prefill(selectedModel());
  }

  function render(status) {
    if (status.defaults) defaults = status.defaults;
    if (status.state === "available") {
      show("available", "Nukleus is available.", "");
      var action = (status.actions || []).filter(function (a) { return a.id === "text-to-image"; })[0];
      if (action) renderAction(action);
    } else {
      actionSection.hidden = true;
      if (status.state === "unavailable") show("unavailable", "Nukleus is unavailable.", REASONS[status.reason] || "Unknown reason.");
      else show("connecting", "Connecting to Nukleus…", "");
    }
  }

  function pollStatus() {
    clearTimeout(statusTimer);
    fetch("/api/status", { cache: "no-store" })
      .then(function (response) {
        if (!response.ok) throw new Error("status " + response.status);
        return response.json();
      })
      .then(render)
      .catch(function () {
        actionSection.hidden = true;
        show("unavailable", "The web service cannot be reached.", "Check your network connection.");
      })
      .then(function () {
        // The model list is read when the page loads and when it is shown again (R3); poll only to recover.
        if (panel.getAttribute("data-state") !== "available") statusTimer = setTimeout(pollStatus, STATUS_POLL_MS);
      });
  }

  // Jobs ---------------------------------------------------------------------------------------

  function element(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function makeCard(job) {
    var item = element("li", "job");
    item.setAttribute("data-state", "queued");
    var heading = element("p", "job-title");
    var info = element("p", "job-detail");
    var bar = document.createElement("progress");
    bar.max = 100;
    bar.hidden = true;
    var media = element("div", "job-media");
    var remove = element("button", "secondary", "Remove");
    remove.type = "button";
    remove.addEventListener("click", function () {
      jobs = jobs.filter(function (other) { return other.id !== job.id; });
      saveJobs();
      item.parentNode.removeChild(item);
      delete cards[job.id];
      jobsSection.hidden = jobs.length === 0;
    });
    heading.textContent = job.title || "Text to image";
    var note = element("p", "job-detail", job.prompt || "");
    [heading, note, info, bar, media, remove].forEach(function (node) { item.appendChild(node); });
    return { item: item, info: info, bar: bar, media: media, done: false };
  }

  function showResult(card, job, status) {
    if (card.media.firstChild) return;
    var url = "/api/jobs/" + job.id + "/result";
    var isVideo = (status.mimeType || "").indexOf("video/") === 0;
    var media = document.createElement(isVideo ? "video" : "img");
    media.src = url;
    if (isVideo) { media.controls = true; media.playsInline = true; } else { media.alt = job.prompt || "Generated image"; }
    var link = element("a", "button", "Download");
    link.href = url;
    link.setAttribute("download", "");
    card.media.appendChild(media);
    card.media.appendChild(link);
  }

  function renderJob(job, card, status) {
    card.item.setAttribute("data-state", status.state);
    card.bar.hidden = status.state !== "running";
    if (status.state === "queued") {
      card.info.textContent = status.queuePosition === null || status.queuePosition === undefined
        ? "Queued." : "Queued, position " + status.queuePosition + ".";
    } else if (status.state === "running") {
      var percent = status.progress || 0;
      card.bar.value = percent;
      card.info.textContent = "Running, " + percent + "%.";
    } else if (status.state === "finished") {
      card.info.textContent = "Finished.";
      showResult(card, job, status);
    } else {
      card.info.textContent = "Failed: " + (status.error || "The job failed.");
    }
    card.done = status.state === "finished" || status.state === "failed";
  }

  function pollJob(job) {
    var card = cards[job.id];
    if (!card || card.done) return;
    fetch("/api/jobs/" + job.id, { cache: "no-store" })
      .then(function (response) {
        if (response.status === 404) {
          // R14, R44, R45: the web service no longer holds it, so there is no state or result.
          card.item.setAttribute("data-state", "gone");
          card.bar.hidden = true;
          card.media.textContent = "";
          card.info.textContent = "No longer available.";
          card.done = true;
          return null;
        }
        if (!response.ok) throw new Error("status " + response.status);
        return response.json();
      })
      .then(function (status) {
        if (!status) return;
        renderJob(job, card, status);
        if (status.state === "failed" && job.id === lastSubmitted) showFormError(status.error || "The job failed.");
      })
      .catch(function () { /* keep the last state and try again */ })
      .then(function () {
        if (cards[job.id] && !cards[job.id].done) setTimeout(function () { pollJob(job); }, JOB_POLL_MS);
      });
  }

  function addCard(job, first) {
    var card = makeCard(job);
    cards[job.id] = card;
    if (first) jobsList.insertBefore(card.item, jobsList.firstChild);
    else jobsList.appendChild(card.item);
    jobsSection.hidden = false;
    pollJob(job);
  }

  // Form ---------------------------------------------------------------------------------------

  var lastSubmitted = null;

  function showFormError(message) {
    formError.textContent = message;
    formError.hidden = !message;
  }

  function onSubmit(event) {
    event.preventDefault();
    showFormError("");
    var model = selectedModel();
    if (!model) { showFormError("Choose a model."); return; }
    submit.disabled = true;
    // The form is left as it is, so a refusal never loses what was entered (R9, R34).
    fetch("/api/jobs", { method: "POST", body: new FormData(form) })
      .then(function (response) {
        if (response.status === 503) throw new Error("The service is busy. Try again in a moment; your values are kept.");
        return response.json().then(function (body) {
          if (!response.ok) throw new Error(body.error || "The job was refused.");
          return body;
        });
      })
      .then(function (body) {
        var job = { id: body.id, title: "Text to image · " + model.name, prompt: form.elements.prompt.value.slice(0, 200) };
        lastSubmitted = job.id;
        jobs.unshift(job);
        saveJobs();
        addCard(job, true);
      })
      .catch(function (error) {
        showFormError(error instanceof SyntaxError ? "The service answered something unexpected." : error.message || "The job could not be sent.");
      })
      .then(function () { submit.disabled = false; });
  }

  function start() {
    // The action is fixed for now: it travels with every job.
    var hidden = document.createElement("input");
    hidden.type = "hidden";
    hidden.name = "action";
    hidden.value = "text-to-image";
    form.appendChild(hidden);

    modelSelect.addEventListener("change", function () { prefill(selectedModel()); });
    form.addEventListener("submit", onSubmit);
    retry.addEventListener("click", pollStatus);
    document.addEventListener("visibilitychange", function () { if (!document.hidden) pollStatus(); });
    jobs.forEach(function (job) { addCard(job, false); });
    jobsSection.hidden = jobs.length === 0;
    pollStatus();
  }

  start();
})();
