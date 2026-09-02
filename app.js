(() => {
  "use strict";

  const DB_NAME = "velocity-cerebras";
  const DB_VERSION = 1;
  const META_STORE = "conversationMeta";
  const CHAT_STORE = "conversations";
  const KEY_PLACEHOLDER = "PASTE_YOUR_CEREBRAS_API_KEY_HERE";
  const API_KEY_STORAGE = "dkchatebras.cerebrasApiKey";
  const API_KEY_STATUS_STORAGE = "dkchatebras.cerebrasApiKeyStatus";

  const config = window.CEREBRAS_CONFIG || {};
  const elements = {
    apiKey: document.querySelector("#api-key"),
    apiState: document.querySelector("#api-state"),
    chatStage: document.querySelector("#chat-stage"),
    closeSettings: document.querySelector("#close-settings"),
    closeSidebar: document.querySelector("#close-sidebar"),
    composer: document.querySelector("#composer"),
    emptyState: document.querySelector("#empty-state"),
    historyCount: document.querySelector("#history-count"),
    historyList: document.querySelector("#history-list"),
    messages: document.querySelector("#messages"),
    model: document.querySelector("#model"),
    openSettings: document.querySelector("#open-settings"),
    newChat: document.querySelector("#new-chat"),
    openSidebar: document.querySelector("#open-sidebar"),
    prompt: document.querySelector("#prompt"),
    removeKey: document.querySelector("#remove-key"),
    send: document.querySelector("#send"),
    sidebar: document.querySelector("#sidebar"),
    settingsDialog: document.querySelector("#settings-dialog"),
    settingsForm: document.querySelector("#settings-form"),
    sidebarScrim: document.querySelector("#sidebar-scrim"),
    suggestions: document.querySelector("#suggestions"),
    toast: document.querySelector("#toast"),
    toggleKey: document.querySelector("#toggle-key"),
  };

  let dbPromise;
  let summaries = [];
  let activeChat = null;
  let activeLoadId = 0;
  let requestController = null;
  let toastTimer = 0;
  let markdownPromise = null;
  const deletedChatIds = new Set();

  function isValidApiKey(value) {
    return typeof value === "string" && value.trim() !== "" && value.trim() !== KEY_PLACEHOLDER;
  }

  function getStoredApiKey() {
    try {
      const storedKey = localStorage.getItem(API_KEY_STORAGE);
      return isValidApiKey(storedKey) ? storedKey.trim() : "";
    } catch {
      return "";
    }
  }

  function getApiKey() {
    const storedKey = getStoredApiKey();
    if (storedKey) return storedKey;
    try {
      if (localStorage.getItem(API_KEY_STATUS_STORAGE) === "removed") return "";
    } catch { /* Fall back to source configuration when storage is unavailable. */ }
    return isValidApiKey(config.apiKey) ? config.apiKey.trim() : "";
  }

  function hasApiKey() {
    return Boolean(getApiKey());
  }

  function setApiState() {
    const ready = hasApiKey();
    elements.apiState.className = `api-state ${ready ? "ready" : "missing"}`;
    elements.apiState.lastElementChild.textContent = ready ? "API key configured" : "API key required";
    elements.removeKey.disabled = !ready;
  }

  function storeApiKey(apiKey) {
    localStorage.setItem(API_KEY_STORAGE, apiKey.trim());
    localStorage.setItem(API_KEY_STATUS_STORAGE, "stored");
  }

  function migrateConfiguredApiKey() {
    if (getStoredApiKey() || !isValidApiKey(config.apiKey)) return;
    try {
      if (localStorage.getItem(API_KEY_STATUS_STORAGE) !== "removed") storeApiKey(config.apiKey);
    } catch { /* The source key remains available for this session. */ }
  }

  function openSettings() {
    elements.apiKey.value = getApiKey();
    elements.apiKey.type = "password";
    elements.toggleKey.textContent = "Show";
    elements.toggleKey.setAttribute("aria-label", "Show API key");
    elements.toggleKey.setAttribute("aria-pressed", "false");
    elements.settingsDialog.showModal();
    elements.apiKey.focus();
  }

  function closeSettings() {
    elements.settingsDialog.close();
  }

  function showToast(message) {
    window.clearTimeout(toastTimer);
    elements.toast.textContent = message;
    elements.toast.classList.add("show");
    toastTimer = window.setTimeout(() => elements.toast.classList.remove("show"), 4200);
  }

  function loadScript(src, globalName) {
    if (window[globalName]) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = src;
      script.onload = resolve;
      script.onerror = () => {
        script.remove();
        reject(new Error(`Could not load ${src}`));
      };
      document.head.append(script);
    });
  }

  function loadMarkdownRenderer() {
    if (!markdownPromise) {
      markdownPromise = Promise.all([
        loadScript("vendor/marked.umd.js", "marked"),
        loadScript("vendor/purify.min.js", "DOMPurify"),
      ]).catch((error) => {
        markdownPromise = null;
        throw error;
      });
    }
    return markdownPromise;
  }

  function isNearBottom() {
    const stage = elements.chatStage;
    return stage.scrollHeight - stage.scrollTop - stage.clientHeight < 48;
  }

  async function renderMarkdown(element, source) {
    try {
      await loadMarkdownRenderer();
      const shouldFollow = element.isConnected && isNearBottom();
      const html = window.marked.parse(source, { gfm: true, breaks: true });
      element.innerHTML = window.DOMPurify.sanitize(html, { USE_PROFILES: { html: true } });
      element.classList.add("markdown");
      if (shouldFollow && element.isConnected) requestAnimationFrame(scrollToBottom);
    } catch (error) {
      console.error("Markdown rendering failed", error);
    }
  }

  function openDatabase() {
    if (!dbPromise) {
      dbPromise = new Promise((resolve, reject) => {
        const request = indexedDB.open(DB_NAME, DB_VERSION);
        request.onupgradeneeded = () => {
          const db = request.result;
          if (!db.objectStoreNames.contains(META_STORE)) {
            const meta = db.createObjectStore(META_STORE, { keyPath: "id" });
            meta.createIndex("updatedAt", "updatedAt");
          }
          if (!db.objectStoreNames.contains(CHAT_STORE)) {
            db.createObjectStore(CHAT_STORE, { keyPath: "id" });
          }
        };
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
    }
    return dbPromise;
  }

  function requestResult(request) {
    return new Promise((resolve, reject) => {
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  }

  function transactionDone(transaction) {
    return new Promise((resolve, reject) => {
      transaction.oncomplete = resolve;
      transaction.onerror = () => reject(transaction.error);
      transaction.onabort = () => reject(transaction.error);
    });
  }

  async function readSummaries() {
    const db = await openDatabase();
    const transaction = db.transaction(META_STORE, "readonly");
    const result = await requestResult(transaction.objectStore(META_STORE).getAll());
    return result.sort((a, b) => b.updatedAt - a.updatedAt);
  }

  async function readChat(id) {
    const db = await openDatabase();
    const transaction = db.transaction(CHAT_STORE, "readonly");
    return requestResult(transaction.objectStore(CHAT_STORE).get(id));
  }

  async function persistChat(chat) {
    const db = await openDatabase();
    const transaction = db.transaction([META_STORE, CHAT_STORE], "readwrite");
    transaction.objectStore(META_STORE).put({
      id: chat.id,
      title: chat.title,
      createdAt: chat.createdAt,
      updatedAt: chat.updatedAt,
      messageCount: chat.messages.length,
    });
    transaction.objectStore(CHAT_STORE).put({
      ...chat,
      messages: chat.messages.map(({ role, content }) => ({ role, content })),
    });
    await transactionDone(transaction);
  }

  async function removeStoredChat(id) {
    const db = await openDatabase();
    const transaction = db.transaction([META_STORE, CHAT_STORE], "readwrite");
    transaction.objectStore(META_STORE).delete(id);
    transaction.objectStore(CHAT_STORE).delete(id);
    await transactionDone(transaction);
  }

  function makeId() {
    return crypto.randomUUID ? crypto.randomUUID() : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }

  function createChat() {
    const now = Date.now();
    return { id: makeId(), title: "New conversation", createdAt: now, updatedAt: now, messages: [] };
  }

  function titleFrom(text) {
    const singleLine = text.replace(/\s+/g, " ").trim();
    return singleLine.length > 46 ? `${singleLine.slice(0, 46).trim()}…` : singleLine;
  }

  function updateSummary(chat) {
    const summary = {
      id: chat.id,
      title: chat.title,
      createdAt: chat.createdAt,
      updatedAt: chat.updatedAt,
      messageCount: chat.messages.length,
    };
    summaries = [summary, ...summaries.filter((item) => item.id !== chat.id)]
      .sort((a, b) => b.updatedAt - a.updatedAt);
    renderHistory();
  }

  function makeIconPath(pathData) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", pathData);
    svg.append(path);
    return svg;
  }

  function renderHistory() {
    const fragment = document.createDocumentFragment();
    elements.historyCount.textContent = String(summaries.length);

    if (!summaries.length) {
      const empty = document.createElement("p");
      empty.className = "empty-history";
      empty.textContent = "Conversations appear here after your first message.";
      fragment.append(empty);
    } else {
      for (const summary of summaries) {
        const row = document.createElement("div");
        row.className = "history-row";

        const select = document.createElement("button");
        select.type = "button";
        select.className = `history-item${activeChat?.id === summary.id ? " active" : ""}`;
        select.dataset.chatId = summary.id;
        select.title = summary.title;
        const title = document.createElement("span");
        title.className = "history-item-title";
        title.textContent = summary.title;
        select.append(title);

        const remove = document.createElement("button");
        remove.type = "button";
        remove.className = "delete-chat";
        remove.dataset.deleteId = summary.id;
        remove.setAttribute("aria-label", `Delete ${summary.title}`);
        remove.append(makeIconPath("M5 7h14M9 7V4h6v3m-8 0 1 13h8l1-13M10 11v5M14 11v5"));

        row.append(select, remove);
        fragment.append(row);
      }
    }

    elements.historyList.replaceChildren(fragment);
  }

  function appendMessage(message) {
    const article = document.createElement("article");
    article.className = `message ${message.role}`;
    if (message.pending) article.classList.add("pending");

    const avatar = document.createElement("div");
    avatar.className = "avatar";
    avatar.textContent = message.role === "assistant" ? "DKC" : "YOU";

    const content = document.createElement("div");
    content.className = "message-content";
    const role = document.createElement("p");
    role.className = "message-role";
    role.textContent = message.role === "assistant" ? "DKChatebras" : "You";
    const text = document.createElement("div");
    text.className = "message-text";
    text.textContent = message.content;
    if (message.role === "assistant" && !message.pending) renderMarkdown(text, message.content);
    content.append(role, text);
    article.append(avatar, content);
    elements.messages.append(article);
    return { article, text };
  }

  function renderChat() {
    const hasMessages = Boolean(activeChat?.messages.length);
    elements.emptyState.hidden = hasMessages;
    elements.messages.classList.toggle("visible", hasMessages);
    elements.messages.replaceChildren();

    if (hasMessages) {
      const fragment = document.createDocumentFragment();
      const target = elements.messages;
      elements.messages = fragment;
      for (const message of activeChat.messages) appendMessage(message);
      elements.messages = target;
      target.append(fragment);
      requestAnimationFrame(scrollToBottom);
    }
  }

  function scrollToBottom() {
    elements.chatStage.scrollTop = elements.chatStage.scrollHeight;
  }

  function closeSidebar() {
    elements.sidebar.classList.remove("open");
    elements.sidebarScrim.hidden = true;
  }

  function cancelCurrentRequest() {
    if (!requestController) return;
    requestController.abort();
    requestController = null;
    setStreaming(false);
  }

  function startNewChat() {
    cancelCurrentRequest();
    activeLoadId += 1;
    activeChat = createChat();
    renderChat();
    renderHistory();
    closeSidebar();
    elements.prompt.focus();
  }

  async function selectChat(id) {
    if (activeChat?.id === id) {
      closeSidebar();
      return;
    }
    cancelCurrentRequest();
    const loadId = ++activeLoadId;
    closeSidebar();

    try {
      const chat = await readChat(id);
      if (loadId !== activeLoadId) return;
      if (!chat) {
        summaries = summaries.filter((item) => item.id !== id);
        renderHistory();
        showToast("That conversation is no longer available.");
        return;
      }
      activeChat = chat;
      renderChat();
      renderHistory();
    } catch (error) {
      showToast(`Could not load conversation: ${error.message}`);
    }
  }

  async function deleteChat(id) {
    deletedChatIds.add(id);
    if (activeChat?.id === id) cancelCurrentRequest();
    try {
      await removeStoredChat(id);
      summaries = summaries.filter((item) => item.id !== id);
      if (activeChat?.id === id) startNewChat();
      else renderHistory();
    } catch (error) {
      deletedChatIds.delete(id);
      showToast(`Could not delete conversation: ${error.message}`);
    }
  }

  function resizePrompt() {
    elements.prompt.style.height = "auto";
    elements.prompt.style.height = `${Math.min(elements.prompt.scrollHeight, 180)}px`;
    elements.send.disabled = !elements.prompt.value.trim() && !requestController;
  }

  function setStreaming(isStreaming) {
    elements.send.classList.toggle("streaming", isStreaming);
    elements.send.disabled = !isStreaming && !elements.prompt.value.trim();
    elements.send.setAttribute("aria-label", isStreaming ? "Stop response" : "Send message");
    elements.model.disabled = isStreaming;
  }

  function parseStreamLine(line, onText) {
    if (!line.startsWith("data:")) return false;
    const data = line.slice(5).trim();
    if (!data || data === "[DONE]") return data === "[DONE]";
    try {
      const event = JSON.parse(data);
      const content = event.choices?.[0]?.delta?.content;
      if (typeof content === "string") onText(content);
    } catch {
      // Ignore incomplete or non-JSON SSE fields; complete data lines continue normally.
    }
    return false;
  }

  async function streamCompletion(messages, model, signal, onText) {
    const apiKey = getApiKey();
    const response = await fetch(config.apiUrl || "https://api.cerebras.ai/v1/chat/completions", {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${apiKey}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ model, messages, stream: true }),
      signal,
    });

    if (!response.ok) {
      const raw = await response.text();
      let detail = raw;
      try {
        const parsed = JSON.parse(raw);
        detail = parsed.error?.message || parsed.message || raw;
      } catch { /* The plain response is already useful. */ }
      throw new Error(detail || `Cerebras returned HTTP ${response.status}`);
    }
    if (!response.body) throw new Error("This browser does not support streamed responses.");

    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";
    let doneEvent = false;

    while (!doneEvent) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      const lines = buffer.split(/\r?\n/);
      buffer = lines.pop() || "";
      for (const line of lines) {
        if (parseStreamLine(line, onText)) {
          doneEvent = true;
          break;
        }
      }
    }

    buffer += decoder.decode();
    if (buffer) parseStreamLine(buffer, onText);
    if (doneEvent) reader.cancel().catch(() => {});
  }

  async function sendMessage(text) {
    if (!hasApiKey()) {
      showToast("Add your Cerebras API key in settings.");
      openSettings();
      return;
    }

    if (!activeChat) activeChat = createChat();
    const chat = activeChat;
    const isFirstMessage = chat.messages.length === 0;
    const userMessage = { role: "user", content: text };
    chat.messages.push(userMessage);
    chat.updatedAt = Date.now();
    if (isFirstMessage) chat.title = titleFrom(text);

    elements.emptyState.hidden = true;
    elements.messages.classList.add("visible");
    appendMessage(userMessage);
    updateSummary(chat);
    persistChat(chat).catch((error) => showToast(`History could not be saved: ${error.message}`));

    const assistantMessage = { role: "assistant", content: "", pending: true };
    chat.messages.push(assistantMessage);
    const assistantView = appendMessage(assistantMessage);
    scrollToBottom();

    elements.prompt.value = "";
    elements.prompt.style.height = "auto";
    requestController = new AbortController();
    const controller = requestController;
    setStreaming(true);

    let paintFrame = null;
    const paint = () => {
      paintFrame = null;
      const shouldFollow = activeChat === chat && isNearBottom();
      assistantView.text.classList.remove("markdown");
      assistantView.text.textContent = assistantMessage.content;
      if (shouldFollow) scrollToBottom();
    };
    const schedulePaint = () => {
      if (paintFrame === null) paintFrame = requestAnimationFrame(paint);
    };
    const flushPaint = () => {
      if (paintFrame === null) return;
      cancelAnimationFrame(paintFrame);
      paint();
    };

    try {
      const requestMessages = chat.messages.slice(0, -1).map(({ role, content }) => ({ role, content }));
      await streamCompletion(requestMessages, elements.model.value, controller.signal, (chunk) => {
        assistantMessage.content += chunk;
        schedulePaint();
      });
      if (!assistantMessage.content) assistantMessage.content = "No text was returned.";
    } catch (error) {
      if (error.name !== "AbortError") showToast(`Request failed: ${error.message}`);
      if (!assistantMessage.content) {
        const index = chat.messages.indexOf(assistantMessage);
        if (index !== -1) chat.messages.splice(index, 1);
        assistantView.article.remove();
      }
    } finally {
      flushPaint();
      assistantMessage.pending = false;
      assistantView.article.classList.remove("pending");
      if (assistantMessage.content) renderMarkdown(assistantView.text, assistantMessage.content);
      chat.updatedAt = Date.now();
      if (requestController === controller) {
        requestController = null;
        setStreaming(false);
        elements.prompt.focus();
      }
      if (!deletedChatIds.has(chat.id)) {
        updateSummary(chat);
        persistChat(chat).catch((error) => showToast(`History could not be saved: ${error.message}`));
      }
    }
  }

  elements.composer.addEventListener("submit", (event) => {
    event.preventDefault();
    if (requestController) {
      requestController.abort();
      return;
    }
    const text = elements.prompt.value.trim();
    if (text) sendMessage(text);
  });

  elements.prompt.addEventListener("input", resizePrompt);
  elements.prompt.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {
      event.preventDefault();
      elements.composer.requestSubmit();
    }
  });

  elements.historyList.addEventListener("click", (event) => {
    const deleteButton = event.target.closest("[data-delete-id]");
    if (deleteButton) {
      deleteChat(deleteButton.dataset.deleteId);
      return;
    }
    const chatButton = event.target.closest("[data-chat-id]");
    if (chatButton) selectChat(chatButton.dataset.chatId);
  });

  elements.suggestions.addEventListener("click", (event) => {
    const suggestion = event.target.closest("[data-prompt]");
    if (!suggestion) return;
    elements.prompt.value = suggestion.dataset.prompt;
    resizePrompt();
    elements.prompt.focus();
  });

  elements.newChat.addEventListener("click", startNewChat);
  elements.openSettings.addEventListener("click", openSettings);
  elements.closeSettings.addEventListener("click", closeSettings);
  elements.settingsDialog.addEventListener("close", () => {
    elements.apiKey.value = "";
    elements.apiKey.type = "password";
    elements.toggleKey.textContent = "Show";
    elements.toggleKey.setAttribute("aria-label", "Show API key");
    elements.toggleKey.setAttribute("aria-pressed", "false");
  });
  elements.toggleKey.addEventListener("click", () => {
    const showing = elements.apiKey.type === "text";
    elements.apiKey.type = showing ? "password" : "text";
    elements.toggleKey.textContent = showing ? "Show" : "Hide";
    elements.toggleKey.setAttribute("aria-label", `${showing ? "Show" : "Hide"} API key`);
    elements.toggleKey.setAttribute("aria-pressed", String(!showing));
    elements.apiKey.focus();
  });
  elements.settingsForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const apiKey = elements.apiKey.value.trim();
    if (!isValidApiKey(apiKey)) {
      showToast("Enter a valid Cerebras API key.");
      elements.apiKey.focus();
      return;
    }
    try {
      storeApiKey(apiKey);
      setApiState();
      closeSettings();
      showToast("API key saved in this browser.");
    } catch (error) {
      showToast(`API key could not be saved: ${error.message}`);
    }
  });
  elements.removeKey.addEventListener("click", () => {
    try {
      localStorage.removeItem(API_KEY_STORAGE);
      localStorage.setItem(API_KEY_STATUS_STORAGE, "removed");
      elements.apiKey.value = "";
      setApiState();
      showToast("API key removed from this browser.");
      elements.apiKey.focus();
    } catch (error) {
      showToast(`API key could not be removed: ${error.message}`);
    }
  });
  elements.openSidebar.addEventListener("click", () => {
    elements.sidebar.classList.add("open");
    elements.sidebarScrim.hidden = false;
  });
  elements.closeSidebar.addEventListener("click", closeSidebar);
  elements.sidebarScrim.addEventListener("click", closeSidebar);
  document.addEventListener("keydown", (event) => {
    if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      startNewChat();
    }
    if (event.key === "Escape") closeSidebar();
  });

  async function initialize() {
    migrateConfiguredApiKey();
    setApiState();
    const preferredModel = config.defaultModel || "gpt-oss-120b";
    if (![...elements.model.options].some((option) => option.value === preferredModel)) {
      elements.model.add(new Option(preferredModel, preferredModel));
    }
    elements.model.value = preferredModel;
    activeChat = createChat();
    resizePrompt();

    try {
      summaries = await readSummaries();
    } catch (error) {
      showToast(`Local history is unavailable: ${error.message}`);
    }
    renderHistory();
  }

  initialize();
})();
