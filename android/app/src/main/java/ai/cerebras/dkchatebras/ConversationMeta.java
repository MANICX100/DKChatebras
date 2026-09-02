package ai.cerebras.dkchatebras;

import org.json.JSONException;
import org.json.JSONObject;

final class ConversationMeta {
    final String id;
    String title;
    String model;
    long updatedAt;

    ConversationMeta(String id, String title, String model, long updatedAt) {
        this.id = id; this.title = title; this.model = model; this.updatedAt = updatedAt;
    }

    JSONObject toJson() throws JSONException {
        return new JSONObject().put("id", id).put("title", title).put("model", model).put("updatedAt", updatedAt);
    }

    static ConversationMeta fromJson(JSONObject json) {
        return new ConversationMeta(json.optString("id"), json.optString("title", "New conversation"),
                json.optString("model", "gpt-oss-120b"), json.optLong("updatedAt", 0));
    }
}
