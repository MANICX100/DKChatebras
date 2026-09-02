package ai.cerebras.dkchatebras;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.InputType;
import android.text.method.LinkMovementMethod;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.window.OnBackInvokedDispatcher;
import android.view.inputmethod.InputMethodManager;
import android.content.Context;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final int SURFACE = Color.rgb(252, 248, 255);
    private static final int SURFACE_CONTAINER = Color.rgb(242, 236, 247);
    private static final int PRIMARY = Color.rgb(91, 75, 219);
    private static final int PRIMARY_CONTAINER = Color.rgb(226, 222, 255);
    private static final int ON_SURFACE = Color.rgb(35, 31, 40);
    private static final int OUTLINE = Color.rgb(121, 116, 126);
    private static final String DEFAULT_MODEL = "gpt-oss-120b";
    private static final long STREAM_HAPTIC_INTERVAL_MS = 80;
    private static final String[] MODELS = {"gpt-oss-120b", "gemma-4-31b"};

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final List<ConversationMeta> index = new ArrayList<>();
    private ConversationStore store;
    private SecureApiKey secureApiKey;
    private CerebrasClient client;
    private ConversationMeta current;
    private List<Message> messages;
    private boolean generating;
    private boolean destroyed;
    private long lastStreamHapticAt;

    private FrameLayout root;
    private View scrim;
    private LinearLayout drawer;
    private LinearLayout historyList;
    private LinearLayout messageList;
    private ScrollView messageScroll;
    private EditText composer;
    private TextView sendButton;
    private TextView modelButton;
    private TextView titleView;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(SURFACE);
        getWindow().setNavigationBarColor(SURFACE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        store = new ConversationStore(this);
        secureApiKey = new SecureApiKey(this);
        client = new CerebrasClient(executor);
        buildInterface();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::handleBack);
        }
        loadInitialData();
        if (!secureApiKey.exists()) root.postDelayed(this::showApiKeyDialog, 350);
    }

    private void buildInterface() {
        root = new FrameLayout(this);
        root.setBackgroundColor(SURFACE);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(0, insets.getSystemWindowInsetTop(), 0, insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(root);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        root.addView(content, match());
        content.addView(buildToolbar(), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(68)));

        messageScroll = new ScrollView(this);
        messageScroll.setFillViewport(true);
        messageScroll.setClipToPadding(false);
        messageScroll.setPadding(dp(12), dp(8), dp(12), dp(12));
        messageList = new LinearLayout(this);
        messageList.setOrientation(LinearLayout.VERTICAL);
        messageList.setGravity(Gravity.BOTTOM);
        messageScroll.addView(messageList, matchWrap());
        content.addView(messageScroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        content.addView(buildComposer(), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        scrim = new View(this);
        scrim.setBackgroundColor(Color.argb(105, 20, 17, 25));
        scrim.setVisibility(View.GONE);
        scrim.setOnClickListener(view -> closeDrawer());
        root.addView(scrim, match());

        drawer = buildDrawer();
        drawer.setVisibility(View.GONE);
        FrameLayout.LayoutParams drawerParams = new FrameLayout.LayoutParams(Math.min(dp(340), getResources().getDisplayMetrics().widthPixels * 86 / 100), ViewGroup.LayoutParams.MATCH_PARENT, Gravity.START);
        root.addView(drawer, drawerParams);
    }

    private View buildToolbar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(8), dp(8), dp(8), dp(8));
        TextView menu = action("☰", 24, false);
        menu.setContentDescription("Open conversation history");
        menu.setOnClickListener(view -> openDrawer());
        bar.addView(menu, new LinearLayout.LayoutParams(dp(52), dp(52)));
        titleView = new TextView(this);
        titleView.setText(R.string.app_name);
        titleView.setTextColor(ON_SURFACE);
        titleView.setTextSize(21);
        titleView.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        titleView.setSingleLine(true);
        titleView.setPadding(dp(8), 0, dp(8), 0);
        bar.addView(titleView, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        modelButton = action("gpt-oss", 12, true);
        modelButton.setOnClickListener(view -> showModelDialog());
        bar.addView(modelButton, new LinearLayout.LayoutParams(dp(92), dp(42)));
        TextView key = action("Key", 13, true);
        key.setContentDescription("Manage Cerebras API key");
        key.setOnClickListener(view -> showApiKeyDialog());
        LinearLayout.LayoutParams keyParams = new LinearLayout.LayoutParams(dp(56), dp(42)); keyParams.setMarginStart(dp(6));
        bar.addView(key, keyParams);
        return bar;
    }

    private View buildComposer() {
        LinearLayout panel = new LinearLayout(this);
        panel.setGravity(Gravity.BOTTOM);
        panel.setPadding(dp(12), dp(8), dp(12), dp(12));
        panel.setBackgroundColor(SURFACE);
        composer = new EditText(this);
        composer.setHint("Message DKChatebras");
        composer.setTextColor(ON_SURFACE);
        composer.setHintTextColor(OUTLINE);
        composer.setTextSize(16);
        composer.setMinLines(1);
        composer.setMaxLines(6);
        composer.setPadding(dp(18), dp(12), dp(18), dp(12));
        composer.setBackground(roundRect(Color.WHITE, 26, OUTLINE, 1));
        composer.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        composer.setImeOptions(EditorInfo.IME_ACTION_SEND);
        composer.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_SEND) { sendMessage(); return true; }
            return false;
        });
        panel.addView(composer, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        sendButton = action("➤", 22, false);
        sendButton.setContentDescription("Send message");
        sendButton.setTextColor(Color.WHITE);
        sendButton.setBackground(roundRect(PRIMARY, 26, PRIMARY, 0));
        sendButton.setOnClickListener(view -> { if (generating) stopGeneration(); else sendMessage(); });
        LinearLayout.LayoutParams sendParams = new LinearLayout.LayoutParams(dp(52), dp(52)); sendParams.setMarginStart(dp(8));
        panel.addView(sendButton, sendParams);
        return panel;
    }

    private LinearLayout buildDrawer() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(16), dp(16), dp(16), dp(16));
        panel.setBackground(roundRect(Color.rgb(248, 243, 252), 0, Color.TRANSPARENT, 0));
        LinearLayout heading = new LinearLayout(this); heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = new TextView(this); name.setText(R.string.conversations); name.setTextColor(ON_SURFACE); name.setTextSize(24); name.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        heading.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView add = action("＋", 26, false); add.setContentDescription("New conversation"); add.setOnClickListener(view -> newConversation());
        heading.addView(add, new LinearLayout.LayoutParams(dp(50), dp(50)));
        panel.addView(heading, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(62)));
        ScrollView historyScroll = new ScrollView(this); historyScroll.setFillViewport(true);
        historyList = new LinearLayout(this); historyList.setOrientation(LinearLayout.VERTICAL);
        historyScroll.addView(historyList, matchWrap());
        panel.addView(historyScroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        TextView apiKey = action("Manage API key", 15, true); apiKey.setGravity(Gravity.CENTER_VERTICAL); apiKey.setPadding(dp(18), 0, dp(18), 0);
        apiKey.setOnClickListener(view -> { closeDrawer(); showApiKeyDialog(); });
        panel.addView(apiKey, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(52)));
        return panel;
    }

    private void loadInitialData() {
        setLoadingState("Loading conversations…");
        executor.execute(() -> {
            try {
                List<ConversationMeta> loaded = store.loadIndex();
                ConversationMeta selected;
                if (loaded.isEmpty()) selected = store.create(loaded, DEFAULT_MODEL); else selected = loaded.get(0);
                List<Message> selectedMessages = store.loadConversation(selected.id);
                runOnUiThread(() -> {
                    index.clear(); index.addAll(loaded); current = selected; messages = selectedMessages;
                    refreshHistory(); renderMessages(); updateHeader();
                });
            } catch (Exception error) { runOnUiThread(() -> showFatalStorageError(error)); }
        });
    }

    private void newConversation() {
        if (generating) { toast("Stop generation before changing conversations"); return; }
        closeDrawer();
        String model = current == null ? DEFAULT_MODEL : current.model;
        executor.execute(() -> {
            try {
                ConversationMeta created = store.create(index, model);
                runOnUiThread(() -> { current = created; messages = new ArrayList<>(); refreshHistory(); renderMessages(); updateHeader(); composer.requestFocus(); });
            } catch (Exception error) { reportStorageError(error); }
        });
    }

    private void selectConversation(ConversationMeta selected) {
        if (selected == current) { closeDrawer(); return; }
        if (generating) { toast("Stop generation before changing conversations"); return; }
        closeDrawer(); setLoadingState("Loading conversation…");
        executor.execute(() -> {
            try {
                List<Message> loaded = store.loadConversation(selected.id);
                runOnUiThread(() -> { current = selected; messages = loaded; renderMessages(); refreshHistory(); updateHeader(); });
            } catch (Exception error) { reportStorageError(error); }
        });
    }

    private void confirmDelete(ConversationMeta meta) {
        if (generating) { toast("Stop generation before deleting conversations"); return; }
        new AlertDialog.Builder(this).setTitle("Delete conversation?").setMessage("This permanently removes “" + meta.title + "”.")
                .setNegativeButton("Cancel", null).setPositiveButton("Delete", (dialog, which) -> deleteConversation(meta)).show();
    }

    private void deleteConversation(ConversationMeta meta) {
        executor.execute(() -> {
            try {
                store.delete(index, meta);
                ConversationMeta replacement = current;
                List<Message> replacementMessages = messages;
                if (meta == current) {
                    if (index.isEmpty()) replacement = store.create(index, DEFAULT_MODEL); else replacement = index.get(0);
                    replacementMessages = store.loadConversation(replacement.id);
                }
                ConversationMeta finalReplacement = replacement; List<Message> finalMessages = replacementMessages;
                runOnUiThread(() -> { current = finalReplacement; messages = finalMessages; refreshHistory(); renderMessages(); updateHeader(); });
            } catch (Exception error) { reportStorageError(error); }
        });
    }

    private void sendMessage() {
        if (generating || current == null || messages == null) return;
        String prompt = composer.getText().toString().trim(); if (prompt.isEmpty()) return;
        final String key;
        try { key = secureApiKey.read(); }
        catch (Exception error) { toast(error.getMessage()); showApiKeyDialog(); return; }
        if (key == null || key.isEmpty()) { showApiKeyDialog(); return; }
        composer.setText("");
        ((InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(composer.getWindowToken(), 0);
        Message user = new Message("user", prompt); Message assistant = new Message("assistant", "");
        messages.add(user);
        if (messages.size() == 1) current.title = titleFrom(prompt);
        current.updatedAt = System.currentTimeMillis();
        saveCurrent();
        messages.add(assistant);
        renderMessages(); refreshHistory(); setGenerating(true);
        ConversationMeta generationConversation = current;
        List<Message> requestMessages = new ArrayList<>(messages.subList(0, messages.size() - 1));
        TextView streamingView = (TextView) messageList.getChildAt(messageList.getChildCount() - 1).findViewWithTag("message");
        client.stream(key, current.model, requestMessages, new CerebrasClient.Listener() {
            @Override public void onDelta(String text) { runOnUiThread(() -> {
                if (destroyed || current != generationConversation || !generating) return;
                assistant.content += text;
                streamingView.setText(assistant.content);
                performStreamHaptic(streamingView);
                scrollToBottom();
            }); }
            @Override public void onComplete() { runOnUiThread(() -> finishGeneration(generationConversation, assistant, streamingView, null)); }
            @Override public void onError(String error) { runOnUiThread(() -> finishGeneration(generationConversation, assistant, streamingView, error)); }
        });
    }

    private void finishGeneration(ConversationMeta conversation, Message assistant, TextView view, String error) {
        if (destroyed || current != conversation || !generating) return;
        if (error != null) {
            if (assistant.content.isEmpty()) assistant.content = "Request failed: " + error;
            toast(error);
        } else if (assistant.content.isEmpty()) assistant.content = "Generation stopped.";
        view.setText(MarkdownRenderer.render(assistant.content));
        view.setMovementMethod(LinkMovementMethod.getInstance());
        conversation.updatedAt = System.currentTimeMillis();
        setGenerating(false); refreshHistory(); saveCurrent(); scrollToBottom();
    }

    private void stopGeneration() { if (generating) { client.stop(); sendButton.setText("…"); sendButton.setContentDescription("Stopping generation"); } }

    private void performStreamHaptic(View streamingView) {
        long now = SystemClock.uptimeMillis();
        if (!streamingView.isAttachedToWindow() || now - lastStreamHapticAt < STREAM_HAPTIC_INTERVAL_MS) return;
        lastStreamHapticAt = now;
        streamingView.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);
    }

    private void saveCurrent() {
        ConversationMeta meta = current; List<Message> snapshot = new ArrayList<>();
        for (Message message : messages) snapshot.add(new Message(message.role, message.content, message.createdAt));
        List<ConversationMeta> indexSnapshot = new ArrayList<>(index);
        executor.execute(() -> { try { store.saveConversation(meta, snapshot); store.saveIndex(indexSnapshot); } catch (Exception error) { reportStorageError(error); } });
    }

    private void renderMessages() {
        messageList.removeAllViews();
        if (messages == null) { setLoadingState("Loading…"); return; }
        if (messages.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText("Ask anything\n\nFast, native chat with Cerebras");
            empty.setTextColor(OUTLINE); empty.setTextSize(20); empty.setGravity(Gravity.CENTER); empty.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            messageList.addView(empty, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(250)));
            return;
        }
        for (Message message : messages) messageList.addView(messageBubble(message));
        scrollToBottom();
    }

    private View messageBubble(Message message) {
        boolean user = "user".equals(message.role);
        LinearLayout row = new LinearLayout(this); row.setGravity(user ? Gravity.END : Gravity.START);
        TextView text = new TextView(this); text.setTag("message"); text.setTextColor(ON_SURFACE); text.setTextSize(16); text.setLineSpacing(0, 1.12f);
        text.setTextIsSelectable(true); text.setPadding(dp(16), dp(12), dp(16), dp(12)); text.setMaxWidth(getResources().getDisplayMetrics().widthPixels * 86 / 100);
        text.setBackground(roundRect(user ? PRIMARY_CONTAINER : Color.WHITE, user ? 24 : 18, Color.TRANSPARENT, 0));
        if (user || (generating && message == messages.get(messages.size() - 1))) text.setText(message.content);
        else { text.setText(MarkdownRenderer.render(message.content)); text.setMovementMethod(LinkMovementMethod.getInstance()); }
        row.addView(text, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rowParams.setMargins(0, dp(5), 0, dp(5)); row.setLayoutParams(rowParams); return row;
    }

    private void refreshHistory() {
        historyList.removeAllViews();
        for (ConversationMeta meta : index) {
            LinearLayout row = new LinearLayout(this); row.setGravity(Gravity.CENTER_VERTICAL); row.setPadding(dp(8), dp(5), dp(4), dp(5));
            if (meta == current) row.setBackground(roundRect(PRIMARY_CONTAINER, 18, Color.TRANSPARENT, 0));
            LinearLayout labels = new LinearLayout(this); labels.setOrientation(LinearLayout.VERTICAL); labels.setPadding(dp(8), dp(6), dp(4), dp(6));
            TextView title = new TextView(this); title.setText(meta.title); title.setTextColor(ON_SURFACE); title.setTextSize(15); title.setSingleLine(true); title.setTypeface(Typeface.DEFAULT, meta == current ? Typeface.BOLD : Typeface.NORMAL);
            TextView date = new TextView(this); date.setText(DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(new Date(meta.updatedAt))); date.setTextColor(OUTLINE); date.setTextSize(11);
            labels.addView(title); labels.addView(date); labels.setOnClickListener(view -> selectConversation(meta));
            row.addView(labels, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            TextView delete = action("×", 22, false); delete.setContentDescription("Delete " + meta.title); delete.setOnClickListener(view -> confirmDelete(meta));
            row.addView(delete, new LinearLayout.LayoutParams(dp(42), dp(42)));
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT); params.setMargins(0, dp(3), 0, dp(3));
            historyList.addView(row, params);
        }
    }

    private void showModelDialog() {
        if (current == null) return;
        int checked = MODELS[1].equals(current.model) ? 1 : 0;
        new AlertDialog.Builder(this).setTitle("Choose model").setSingleChoiceItems(MODELS, checked, (dialog, which) -> {
            current.model = MODELS[which]; current.updatedAt = System.currentTimeMillis(); updateHeader(); saveCurrent(); dialog.dismiss();
        }).setNegativeButton("Cancel", null).show();
    }

    private void showApiKeyDialog() {
        EditText input = new EditText(this); input.setHint(secureApiKey.exists() ? "Enter replacement key" : "csk-…");
        input.setSingleLine(true); input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD); input.setPadding(dp(18), dp(12), dp(18), dp(12));
        FrameLayout holder = new FrameLayout(this); holder.setPadding(dp(20), dp(4), dp(20), 0);
                holder.addView(input, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        AlertDialog.Builder builder = new AlertDialog.Builder(this).setTitle("Cerebras API key")
                .setMessage("Encrypted with an Android Keystore-backed AES-GCM key. The key is sent only to api.cerebras.ai.")
                .setView(holder).setNegativeButton("Cancel", null).setPositiveButton(secureApiKey.exists() ? "Update" : "Save", null);
        if (secureApiKey.exists()) builder.setNeutralButton("Remove", (dialog, which) -> removeApiKey());
        AlertDialog dialog = builder.create();
        dialog.setOnShowListener(unused -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
            String value = input.getText().toString().trim(); if (value.isEmpty()) { input.setError("Enter an API key"); return; }
            try { secureApiKey.save(value); toast("API key saved securely"); dialog.dismiss(); }
            catch (Exception error) { input.setError(error.getMessage()); }
        }));
        dialog.show();
    }

    private void removeApiKey() {
        if (generating) stopGeneration();
        try { secureApiKey.remove(); toast("API key removed"); } catch (Exception error) { toast("Could not remove API key: " + error.getMessage()); }
    }

    private void updateHeader() {
        if (current == null) return;
        titleView.setText(R.string.app_name);
        modelButton.setText(current.model.equals(DEFAULT_MODEL) ? "gpt-oss" : "gemma-4");
    }

    private void setGenerating(boolean value) {
        generating = value;
        if (value) lastStreamHapticAt = 0;
        composer.setEnabled(!value); modelButton.setEnabled(!value);
        sendButton.setText(value ? "■" : "➤"); sendButton.setContentDescription(value ? "Stop generation" : "Send message");
        sendButton.setBackground(roundRect(value ? Color.rgb(184, 49, 70) : PRIMARY, 26, Color.TRANSPARENT, 0));
    }

    private void setLoadingState(String label) { messageList.removeAllViews(); TextView loading = new TextView(this); loading.setText(label); loading.setTextColor(OUTLINE); loading.setGravity(Gravity.CENTER); messageList.addView(loading, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(220))); }
    private void scrollToBottom() { messageScroll.post(() -> messageScroll.fullScroll(View.FOCUS_DOWN)); }
    private void openDrawer() { refreshHistory(); scrim.setVisibility(View.VISIBLE); drawer.setVisibility(View.VISIBLE); drawer.setTranslationX(-drawer.getWidth()); drawer.animate().translationX(0).setDuration(180).start(); }
    private void closeDrawer() { drawer.animate().translationX(-drawer.getWidth()).setDuration(150).withEndAction(() -> { drawer.setVisibility(View.GONE); scrim.setVisibility(View.GONE); }).start(); }
    private String titleFrom(String text) { String oneLine = text.replace('\n', ' ').trim(); return oneLine.length() <= 42 ? oneLine : oneLine.substring(0, 39) + "…"; }
    private void toast(String message) { Toast.makeText(this, message, Toast.LENGTH_LONG).show(); }
    private void reportStorageError(Exception error) { runOnUiThread(() -> toast("Storage error: " + error.getMessage())); }
    private void showFatalStorageError(Exception error) { setLoadingState("Could not load conversations.\n" + error.getMessage()); }

    private TextView action(String text, float size, boolean filled) {
        TextView view = new TextView(this); view.setText(text); view.setTextSize(size); view.setTextColor(filled ? PRIMARY : ON_SURFACE); view.setGravity(Gravity.CENTER); view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        view.setBackground(roundRect(filled ? SURFACE_CONTAINER : Color.TRANSPARENT, 20, Color.TRANSPARENT, 0)); view.setClickable(true); view.setFocusable(true); return view;
    }
    private GradientDrawable roundRect(int color, int radiusDp, int strokeColor, int strokeDp) {
        GradientDrawable drawable = new GradientDrawable(); drawable.setColor(color); drawable.setCornerRadius(dp(radiusDp)); if (strokeDp > 0) drawable.setStroke(dp(strokeDp), strokeColor); return drawable;
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private static FrameLayout.LayoutParams match() { return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT); }
    private static LinearLayout.LayoutParams matchWrap() { return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT); }

    private void handleBack() {
        if (drawer.getVisibility() == View.VISIBLE) closeDrawer(); else finishAfterTransition();
    }

    @SuppressLint("GestureBackNavigation")
    @Override public void onBackPressed() { handleBack(); }
    @Override protected void onDestroy() {
        destroyed = true;
        if (generating && current != null && messages != null) {
            Message assistant = messages.get(messages.size() - 1);
            if (assistant.content.isEmpty()) assistant.content = "Generation stopped.";
            saveCurrent();
        }
        if (client != null) client.stop();
        executor.shutdown();
        super.onDestroy();
    }
}
