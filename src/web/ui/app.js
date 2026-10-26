(function () {
  "use strict";

  var POLL_MS = 3000;
  var REASONS = {
    "unreachable": "The Nukleus service cannot be reached. Check that it is running and that this service points at the right host.",
    "authentication-refused": "The Nukleus service refused this service's credentials. Check the backend token.",
    "incompatible-protocol": "The Nukleus service speaks a protocol version this service does not support. Update both to the same release."
  };

  var panel = document.getElementById("status");
  var title = document.getElementById("status-title");
  var detail = document.getElementById("status-detail");
  var retry = document.getElementById("retry");
  var timer = null;

  function show(state, text, extra) {
    panel.setAttribute("data-state", state);
    title.textContent = text;
    detail.textContent = extra || "";
    retry.hidden = state !== "unavailable";
  }

  function render(status) {
    if (status.state === "available") {
      show("available", "Nukleus is available.", "");
    } else if (status.state === "unavailable") {
      show("unavailable", "Nukleus is unavailable.", REASONS[status.reason] || "Unknown reason.");
    } else {
      show("connecting", "Connecting to Nukleus…", "");
    }
  }

  function poll() {
    clearTimeout(timer);
    fetch("/api/status", { cache: "no-store" })
      .then(function (response) {
        if (!response.ok) throw new Error("status " + response.status);
        return response.json();
      })
      .then(render)
      .catch(function () {
        show("unavailable", "The web service cannot be reached.", "Check your network connection.");
      })
      .then(function () {
        timer = setTimeout(poll, POLL_MS);
      });
  }

  retry.addEventListener("click", poll);
  poll();
})();
