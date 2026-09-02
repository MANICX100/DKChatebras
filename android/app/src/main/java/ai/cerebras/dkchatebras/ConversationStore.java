package ai.cerebras.dkchatebras;

import android.content.Context;
import android.util.AtomicFile;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

final class ConversationStore {
    private final File directory;
    private final File indexFile;

    ConversationStore(Context context) {
        directory = new File(context.getFilesDir(), "conversations");
        if (!directory.exists() && !directory.mkdirs()) throw new IllegalStateException("Cannot create conversation storage");
        indexFile = new File(directory, "index.json");
    }

    synchronized List<ConversationMeta> loadIndex() throws Exception {
        ArrayList<ConversationMeta> result = new ArrayList<>();
        if (indexFile.exists()) {
            JSONArray array = new JSONArray(read(indexFile));
            for (int i = 0; i < array.length(); i++) result.add(ConversationMeta.fromJson(array.getJSONObject(i)));
        }
        sort(result); return result;
    }

    synchronized ConversationMeta create(List<ConversationMeta> index, String model) throws Exception {
        ConversationMeta meta = new ConversationMeta(UUID.randomUUID().toString(), "New conversation", model, System.currentTimeMillis());
        index.add(meta);
        try { saveConversation(meta, new ArrayList<>()); saveIndex(index); }
        catch (Exception error) { index.remove(meta); throw error; }
        return meta;
    }

    synchronized List<Message> loadConversation(String id) throws Exception {
        ArrayList<Message> result = new ArrayList<>();
        File file = conversationFile(id); if (!file.exists()) return result;
        JSONArray messages = new JSONObject(read(file)).optJSONArray("messages");
        if (messages != null) for (int i = 0; i < messages.length(); i++) result.add(Message.fromJson(messages.getJSONObject(i)));
        return result;
    }

    synchronized void saveConversation(ConversationMeta meta, List<Message> messages) throws Exception {
        JSONArray array = new JSONArray(); for (Message message : messages) array.put(message.toJson());
        writeAtomic(conversationFile(meta.id), new JSONObject().put("id", meta.id).put("model", meta.model).put("messages", array).toString());
    }

    synchronized void saveIndex(List<ConversationMeta> index) throws Exception {
        sort(index); JSONArray array = new JSONArray(); for (ConversationMeta meta : index) array.put(meta.toJson());
        writeAtomic(indexFile, array.toString());
    }

    synchronized void delete(List<ConversationMeta> index, ConversationMeta meta) throws Exception {
        int position = index.indexOf(meta);
        if (position < 0) return;
        index.remove(position);
        try { saveIndex(index); }
        catch (Exception error) { index.add(position, meta); throw error; }
        File file = conversationFile(meta.id);
        if (!file.delete() && file.exists()) {
            index.add(meta);
            saveIndex(index);
            throw new IllegalStateException("Cannot delete conversation");
        }
    }

    private File conversationFile(String id) {
        if (!id.matches("[a-fA-F0-9-]+")) throw new IllegalArgumentException("Invalid conversation identifier");
        return new File(directory, id + ".json");
    }

    private static void sort(List<ConversationMeta> index) { Collections.sort(index, (a, b) -> Long.compare(b.updatedAt, a.updatedAt)); }

    private static String read(File file) throws Exception {
        StringBuilder text = new StringBuilder();
        try (Reader reader = new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8)) {
            char[] buffer = new char[4096]; int count; while ((count = reader.read(buffer)) != -1) text.append(buffer, 0, count);
        } return text.toString();
    }

    private static void writeAtomic(File file, String value) throws Exception {
        AtomicFile atomicFile = new AtomicFile(file);
        FileOutputStream stream = null;
        try {
            stream = atomicFile.startWrite();
            Writer writer = new OutputStreamWriter(stream, StandardCharsets.UTF_8);
            writer.write(value); writer.flush();
            atomicFile.finishWrite(stream);
        } catch (Exception error) {
            if (stream != null) atomicFile.failWrite(stream);
            throw error;
        }
    }
}
