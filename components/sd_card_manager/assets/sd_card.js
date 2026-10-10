"use strict";

(() => {
  const byId = (id) => document.getElementById(id);
  let currentPath = "/";
  let nextOffset = 0;
  let hasMore = false;
  let busy = false;
  let mounted = false;
  let csrfToken = "";

  function showError(message) {
    byId("sd-action-message").classList.add("d-none");
    const element = byId("sd-error");
    element.textContent = message;
    element.classList.remove("d-none");
  }

  function showAction(message, isError = false) {
    byId("sd-error").classList.add("d-none");
    const element = byId("sd-action-message");
    element.textContent = message;
    element.className = `alert ${isError ? "alert-danger" : "alert-success"}`;
  }

  function updateControls() {
    for (const id of ["sd-new-file", "sd-new-folder", "sd-upload", "sd-refresh"]) {
      byId(id).disabled = busy || !mounted;
    }
    byId("sd-next-page").disabled = busy;
  }

  function formatBytes(value) {
    if (!Number.isFinite(value) || value < 0) return "-";
    if (value < 1024) return `${value} B`;
    const units = ["KiB", "MiB", "GiB", "TiB"];
    let amount = value;
    let unit = -1;
    do {
      amount /= 1024;
      unit += 1;
    } while (amount >= 1024 && unit < units.length - 1);
    return `${amount.toLocaleString("de-DE", { maximumFractionDigits: 1 })} ${units[unit]}`;
  }

  function apiPath(path) {
    return `/api/sd/list?path=${encodeURIComponent(path)}`;
  }

  async function loadStatus() {
    const response = await fetch("/api/sd/status", { cache: "no-store" });
    if (!response.ok) throw new Error(await response.text());
    const status = await response.json();
    mounted = status.mounted;
    byId("sd-status-message").textContent = status.message;
    byId("sd-capacity").textContent = mounted ?
      (status.freeSpaceKnown ?
        `${formatBytes(status.freeBytes)} frei von ${formatBytes(status.totalBytes)}` :
        `Freier Speicher unbekannt (Gesamt: ${formatBytes(status.totalBytes)})`) :
      "Speicher nicht verfuegbar";
    updateControls();
  }

  function renderBreadcrumb() {
    const nav = byId("sd-breadcrumb");
    nav.replaceChildren();
    const list = document.createElement("ol");
    list.className = "breadcrumb mb-0";
    const segments = currentPath.split("/").filter(Boolean);
    const paths = ["/"];
    let accumulated = "";
    for (const segment of segments) {
      accumulated += `/${segment}`;
      paths.push(accumulated);
    }
    paths.forEach((path, index) => {
      const item = document.createElement("li");
      item.className = `breadcrumb-item${index === paths.length - 1 ? " active" : ""}`;
      if (index === paths.length - 1) {
        item.textContent = index === 0 ? "SD-Karte" : segments[index - 1];
        item.setAttribute("aria-current", "page");
      } else {
        const link = document.createElement("button");
        link.type = "button";
        link.className = "btn btn-link btn-sm p-0 text-decoration-none";
        link.textContent = index === 0 ? "SD-Karte" : segments[index - 1];
        link.addEventListener("click", () => void loadDirectory(path, false));
        item.append(link);
      }
      list.append(item);
    });
    nav.append(list);
  }

  function fullChildPath(name) {
    return `${currentPath === "/" ? "" : currentPath}/${name}`;
  }

  function addActionButton(row, label, className, action) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `btn btn-sm ${className} ms-1`;
    button.textContent = label;
    button.disabled = busy;
    button.addEventListener("click", action);
    row.append(button);
  }

  function renderEntry(item) {
    const row = document.createElement("tr");
    const nameCell = document.createElement("td");
    const name = document.createElement(item.directory ? "button" : "span");
    if (item.directory) {
      name.type = "button";
      name.className = "btn btn-link p-0 text-decoration-none fw-semibold";
      name.addEventListener("click", () => void loadDirectory(fullChildPath(item.name), false));
    } else {
      name.className = "text-break";
    }
    const kind = document.createElement("span");
    kind.className = `badge me-2 ${item.directory ? "text-bg-secondary" : "text-bg-light"}`;
    kind.textContent = item.directory ? "Ordner" : "Datei";
    nameCell.append(kind);
    name.textContent = item.name;
    nameCell.append(name);
    const sizeCell = document.createElement("td");
    sizeCell.className = "text-end text-nowrap";
    sizeCell.textContent = item.directory ? "-" : formatBytes(item.size);
    const actions = document.createElement("td");
    actions.className = "text-end text-nowrap";
    if (!item.directory) {
      addActionButton(actions, "Download", "btn-outline-primary", () => {
        window.location.assign(`/api/sd/download?path=${encodeURIComponent(fullChildPath(item.name))}`);
      });
    }
    addActionButton(actions, "Umbenennen", "btn-outline-secondary", () => {
      const nameValue = prompt("Neuer Name:", item.name);
      if (!nameValue || nameValue === item.name) return;
      const parent = currentPath === "/" ? "" : currentPath;
      void mutate("/api/sd/move", {
        source: fullChildPath(item.name),
        destination: `${parent}/${nameValue}`,
      });
    });
    addActionButton(actions, "Verschieben", "btn-outline-secondary", () => {
      const destination = prompt("Absoluter Zielpfad, z. B. /Archiv/datei.bin:",
        fullChildPath(item.name));
      if (!destination || destination === fullChildPath(item.name)) return;
      void mutate("/api/sd/move", { source: fullChildPath(item.name), destination });
    });
    addActionButton(actions, "Loeschen", "btn-outline-danger", () => {
      if (confirm(`"${item.name}" endgueltig loeschen?`)) {
        void mutate("/api/sd/delete", { path: fullChildPath(item.name) });
      }
    });
    row.append(nameCell, sizeCell, actions);
    return row;
  }

  async function loadDirectory(path, append) {
    if (busy || !mounted) return;
    busy = true;
    updateControls();
    byId("sd-error").classList.add("d-none");
    try {
      const offset = append ? nextOffset : 0;
      const response = await fetch(`${apiPath(path)}&offset=${offset}`, { cache: "no-store" });
      if (!response.ok) throw new Error(await response.text());
      const result = await response.json();
      currentPath = result.path;
      nextOffset = result.nextOffset;
      hasMore = result.hasMore;
      if (!append) byId("sd-file-list").replaceChildren();
      for (const entry of result.items) byId("sd-file-list").append(renderEntry(entry));
      if (!append && result.items.length === 0) {
        const empty = document.createElement("tr");
        const cell = document.createElement("td");
        cell.colSpan = 3;
        cell.className = "text-body-secondary";
        cell.textContent = "Dieser Ordner ist leer.";
        empty.append(cell);
        byId("sd-file-list").append(empty);
      }
      byId("sd-item-count").textContent = append ?
        `${byId("sd-file-list").children.length} Eintraege angezeigt` :
        `${result.items.length} Eintraege angezeigt`;
      byId("sd-next-page").classList.toggle("d-none", !hasMore);
      renderBreadcrumb();
    } catch (error) {
      showError(`Verzeichnis konnte nicht geladen werden: ${error.message}`);
    } finally {
      busy = false;
      updateControls();
    }
  }

  async function mutate(url, body) {
    if (busy || !mounted) return;
    busy = true;
    updateControls();
    byId("sd-error").classList.add("d-none");
    try {
      const response = await fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": csrfToken },
        body: JSON.stringify(body),
      });
      if (!response.ok) throw new Error(await response.text());
      showAction("Aktion erfolgreich.");
      busy = false;
      updateControls();
      await loadStatus();
      await loadDirectory(currentPath, false);
    } catch (error) {
      showError(`Aktion fehlgeschlagen: ${error.message}`);
    } finally {
      busy = false;
      updateControls();
    }
  }

  async function uploadFiles(files) {
    if (busy || !mounted || files.length === 0) return;
    busy = true;
    updateControls();
    byId("sd-error").classList.add("d-none");
    const progress = byId("sd-upload-progress");
    progress.classList.remove("d-none");
    progress.value = 0;
    try {
      for (let index = 0; index < files.length; index += 1) {
        const file = files[index];
        const url = `/api/sd/upload?path=${encodeURIComponent(fullChildPath(file.name))}`;
        await new Promise((resolve, reject) => {
          const request = new XMLHttpRequest();
          request.open("POST", url);
          request.setRequestHeader("X-CSRF-Token", csrfToken);
          request.upload.onprogress = (event) => {
            if (event.lengthComputable) {
              const total = files.length;
              const complete = index / total;
              const current = event.loaded / event.total / total;
              const value = Math.floor((complete + current) * 100);
              progress.value = value;
              progress.setAttribute("aria-valuenow", String(value));
            }
          };
          request.onload = () => request.status >= 200 && request.status < 300 ?
            resolve() : reject(new Error(request.responseText || `HTTP ${request.status}`));
          request.onerror = () => reject(new Error("Netzwerkfehler waehrend des Uploads."));
          request.send(file);
        });
      }
      progress.value = 100;
      progress.setAttribute("aria-valuenow", "100");
      showAction("Upload abgeschlossen.");
      busy = false;
      updateControls();
      await loadStatus();
      await loadDirectory(currentPath, false);
    } catch (error) {
      showError(`Upload fehlgeschlagen: ${error.message}`);
    } finally {
      progress.classList.add("d-none");
      progress.value = 0;
      byId("sd-upload").value = "";
      busy = false;
      updateControls();
    }
  }

  async function initialize() {
    try {
      const response = await fetch("/api/status", { cache: "no-store" });
      if (!response.ok) throw new Error(await response.text());
      csrfToken = (await response.json()).csrfToken;
      await loadStatus();
      if (mounted) await loadDirectory("/", false);
    } catch (error) {
      showError(`SD-Kartenstatus konnte nicht geladen werden: ${error.message}`);
    }
  }

  byId("sd-refresh").addEventListener("click", async () => {
    try {
      await loadStatus();
      if (mounted) await loadDirectory(currentPath, false);
    } catch (error) {
      showError(`Aktualisierung fehlgeschlagen: ${error.message}`);
    }
  });
  byId("sd-next-page").addEventListener("click", () => void loadDirectory(currentPath, true));
  byId("sd-new-file").addEventListener("click", () => {
    const name = prompt("Name der neuen leeren Datei:");
    if (name) void mutate("/api/sd/create", { path: currentPath, name });
  });
  byId("sd-new-folder").addEventListener("click", () => {
    const name = prompt("Name des neuen Ordners:");
    if (name) void mutate("/api/sd/mkdir", { path: fullChildPath(name) });
  });
  byId("sd-upload").addEventListener("change", (event) => {
    void uploadFiles(Array.from(event.target.files));
  });
  void initialize();
})();
