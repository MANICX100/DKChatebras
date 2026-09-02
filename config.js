// Optional legacy fallback. Prefer saving the Cerebras API key in the UI.
// A valid source key is migrated to localStorage unless the user removed it.
window.CEREBRAS_CONFIG = Object.freeze({
  apiKey: "",
  defaultModel: "gpt-oss-120b",
  apiUrl: "https://api.cerebras.ai/v1/chat/completions",
});
