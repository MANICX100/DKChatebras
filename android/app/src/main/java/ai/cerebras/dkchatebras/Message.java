package ai.cerebras.dkchatebras;

import org.json.JSONException;
import org.json.JSONObject;

final class Message {
    final String role;
    String content;
    final long createdAt;

    Message(String role, String content) { this(role, content, System.currentTimeMillis()); }
    Message(String role, String content, long createdAt) { this.role = role; this.content = content; this.createdAt = createdAt; }

    JSONObject toJson() throws JSONException {
        return new JSONObject().put("role", role).put("content", content).put("createdAt", createdAt);
    }

    static Message fromJson(JSONObject json) {
        return new Message(json.optString("role", "assistant"), json.optString("content", ""), json.optLong("createdAt", 0));
    }
}
