package ai.cerebras.dkchatebras;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.ExecutorService;

final class CerebrasClient {
    interface Listener { void onDelta(String text); void onComplete(); void onError(String message); }
    private static final String ENDPOINT = "https://api.cerebras.ai/v1/chat/completions";
    private final ExecutorService executor;
    private volatile HttpURLConnection activeConnection;
    private volatile boolean stopped;

    CerebrasClient(ExecutorService executor) { this.executor = executor; }
    void stream(String apiKey, String model, List<Message> messages, Listener listener) {
        stopped = false; executor.execute(() -> execute(apiKey, model, messages, listener));
    }
    void stop() { stopped = true; HttpURLConnection connection = activeConnection; if (connection != null) connection.disconnect(); }

    private void execute(String apiKey, String model, List<Message> messages, Listener listener) {
        HttpURLConnection connection = null;
        try {
            if (stopped) { listener.onComplete(); return; }
            JSONArray apiMessages = new JSONArray();
            for (Message message : messages) apiMessages.put(new JSONObject().put("role", message.role).put("content", message.content));
            byte[] body = new JSONObject().put("model", model).put("stream", true).put("messages", apiMessages)
                    .toString().getBytes(StandardCharsets.UTF_8);
            connection = (HttpURLConnection) new URL(ENDPOINT).openConnection(); activeConnection = connection;
            if (stopped) { listener.onComplete(); return; }
            connection.setRequestMethod("POST"); connection.setConnectTimeout(20_000); connection.setReadTimeout(60_000);
            connection.setDoOutput(true); connection.setRequestProperty("Authorization", "Bearer " + apiKey);
            connection.setRequestProperty("Content-Type", "application/json"); connection.setRequestProperty("Accept", "text/event-stream");
            connection.setFixedLengthStreamingMode(body.length);
            try (OutputStream output = connection.getOutputStream()) { output.write(body); }
            int status = connection.getResponseCode();
            if (status < 200 || status >= 300) {
                String detail = readAll(connection.getErrorStream());
                throw new IOException("Cerebras returned HTTP " + status + (detail.isEmpty() ? "" : ": " + apiError(detail)));
            }
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(connection.getInputStream(), StandardCharsets.UTF_8))) {
                String line; StringBuilder eventData = null; boolean done = false;
                while (!stopped && !done && (line = reader.readLine()) != null) {
                    if (line.isEmpty()) {
                        if (eventData != null) { done = deliverEvent(eventData.toString(), listener); eventData = null; }
                    } else if (line.equals("data") || line.startsWith("data:")) {
                        String value = line.length() == 4 ? "" : line.substring(5);
                        if (value.startsWith(" ")) value = value.substring(1);
                        if (eventData == null) eventData = new StringBuilder(); else eventData.append('\n');
                        eventData.append(value);
                    }
                }
                if (!stopped && !done && eventData != null) deliverEvent(eventData.toString(), listener);
            }
            listener.onComplete();
        } catch (Exception error) {
            if (stopped) listener.onComplete(); else listener.onError(friendlyMessage(error));
        } finally { activeConnection = null; if (connection != null) connection.disconnect(); }
    }

    private static boolean deliverEvent(String data, Listener listener) throws Exception {
        if ("[DONE]".equals(data.trim())) return true;
        JSONArray choices = new JSONObject(data).optJSONArray("choices");
        if (choices == null || choices.length() == 0) return false;
        JSONObject delta = choices.getJSONObject(0).optJSONObject("delta");
        if (delta != null) {
            String content = delta.optString("content", "");
            if (!content.isEmpty()) listener.onDelta(content);
        }
        return false;
    }

    private static String apiError(String raw) {
        try { JSONObject error = new JSONObject(raw).optJSONObject("error"); return error == null ? raw : error.optString("message", raw); }
        catch (Exception ignored) { return raw; }
    }
    private static String friendlyMessage(Exception error) {
        String message = error.getMessage();
        if (message == null || message.trim().isEmpty()) return "The request failed. Check your connection and try again.";
        if (message.contains("timed out")) return "The request timed out. Check your connection and try again."; return message;
    }
    private static String readAll(InputStream stream) throws IOException {
        if (stream == null) return ""; StringBuilder result = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
            String line; while ((line = reader.readLine()) != null) result.append(line);
        } return result.toString();
    }
}
