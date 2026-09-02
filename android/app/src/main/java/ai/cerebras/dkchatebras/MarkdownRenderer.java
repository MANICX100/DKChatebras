package ai.cerebras.dkchatebras;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.net.Uri;

import android.text.Spannable;
import android.text.SpannableStringBuilder;
import android.text.style.BackgroundColorSpan;
import android.text.style.BulletSpan;
import android.text.style.ForegroundColorSpan;
import android.text.style.LeadingMarginSpan;
import android.text.style.QuoteSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.ReplacementSpan;
import android.text.style.StrikethroughSpan;
import android.text.style.StyleSpan;
import android.text.style.TypefaceSpan;
import android.text.style.URLSpan;


import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class MarkdownRenderer {
    private static final int ACCENT = Color.rgb(68, 56, 180);
    private static final int CODE_BACKGROUND = Color.rgb(235, 230, 242);
    private static final int CODE_TEXT = Color.rgb(45, 40, 54);
    private static final int RULE_COLOR = Color.rgb(190, 183, 198);
    private static final int SPAN_FLAGS = Spannable.SPAN_EXCLUSIVE_EXCLUSIVE;
    private static final Pattern LIST_ITEM = Pattern.compile("^(\\s*)([-+*]|(\\d+)[.)])\\s+(.*)$");
    private static final Pattern FENCE = Pattern.compile("^ {0,3}(`{3,}|~{3,})(.*)$");
    private static final String ESCAPABLE = "\\`*{}_[]()#+-.!>|~";

    private MarkdownRenderer() {}

    static SpannableStringBuilder render(String markdown) {
        SpannableStringBuilder output = new SpannableStringBuilder();
        String[] lines = markdown.replace("\r\n", "\n").replace('\r', '\n').split("\n", -1);
        char fenceMarker = 0;
        int fenceLength = 0;
        int codeStart = -1;

        for (String source : lines) {
            Matcher fence = FENCE.matcher(source);
            if (fenceMarker != 0) {
                if (fence.matches() && fence.group(1).charAt(0) == fenceMarker
                        && fence.group(1).length() >= fenceLength
                        && fence.group(2).trim().isEmpty()) {
                    styleCode(output, codeStart, endWithoutTrailingNewline(output));
                    fenceMarker = 0;
                    codeStart = -1;
                } else {
                    output.append(source).append('\n');
                }
                continue;
            }
            if (fence.matches()) {
                fenceMarker = fence.group(1).charAt(0);
                fenceLength = fence.group(1).length();
                codeStart = output.length();
                continue;
            }
            appendBlockLine(output, source);
        }

        if (fenceMarker != 0) styleCode(output, codeStart, endWithoutTrailingNewline(output));
        trimLastNewline(output);
        return output;
    }

    private static void appendBlockLine(SpannableStringBuilder output, String source) {
        int paragraphStart = output.length();
        String line = source;
        int quoteDepth = 0;
        while (true) {
            int index = leadingSpaces(line);
            if (index > 3 || index >= line.length() || line.charAt(index) != '>') break;
            quoteDepth++;
            index++;
            if (index < line.length() && line.charAt(index) == ' ') index++;
            line = line.substring(index);
        }

        int heading = headingLevel(line);
        if (heading > 0) line = line.substring(leadingSpaces(line) + heading + 1).replaceFirst("\\s+#+\\s*$", "");

        Matcher list = LIST_ITEM.matcher(line);
        boolean listItem = list.matches();
        boolean unordered = false;
        int listIndent = 0;
        String orderedPrefix = null;
        if (listItem) {
            listIndent = indentationWidth(list.group(1));
            unordered = list.group(3) == null;
            if (unordered) {
                line = list.group(4);
            } else {
                orderedPrefix = list.group(2) + "  ";
                line = list.group(4);
            }
        }

        if (quoteDepth == 0 && heading == 0 && !listItem && isHorizontalRule(line)) {
            int start = output.length();
            output.append('\uFFFC').append('\n');
            output.setSpan(new HorizontalRuleSpan(), start, start + 1, SPAN_FLAGS);
            return;
        }

        if (orderedPrefix != null) output.append(orderedPrefix);
        int contentStart = output.length();
        appendInline(output, line, 0, line.length());
        int contentEnd = output.length();
        output.append('\n');
        int paragraphEnd = output.length();

        if (heading > 0 && contentEnd > contentStart) {
            float[] scales = {1.55f, 1.38f, 1.24f, 1.14f, 1.07f, 1.0f};
            output.setSpan(new RelativeSizeSpan(scales[heading - 1]), contentStart, contentEnd, SPAN_FLAGS);
            output.setSpan(new StyleSpan(Typeface.BOLD), contentStart, contentEnd, SPAN_FLAGS);
        }
        if (unordered) {
            int margin = 24 + (listIndent / 2) * 12;
            output.setSpan(new LeadingMarginSpan.Standard(margin), paragraphStart, paragraphEnd, SPAN_FLAGS);
            output.setSpan(new BulletSpan(10, ACCENT), paragraphStart, paragraphEnd, SPAN_FLAGS);
        } else if (orderedPrefix != null) {
            int margin = (listIndent / 2) * 12;
            if (margin > 0) output.setSpan(new LeadingMarginSpan.Standard(margin), paragraphStart, paragraphEnd, SPAN_FLAGS);
        }
        if (quoteDepth > 0) {
            output.setSpan(new QuoteSpan(ACCENT), paragraphStart, paragraphEnd, SPAN_FLAGS);
            output.setSpan(new LeadingMarginSpan.Standard(quoteDepth * 12), paragraphStart, paragraphEnd, SPAN_FLAGS);
        }
    }

    private static void appendInline(SpannableStringBuilder output, String text, int from, int to) {
        int position = from;
        while (position < to) {
            char current = text.charAt(position);
            if (current == '\\' && position + 1 < to && ESCAPABLE.indexOf(text.charAt(position + 1)) >= 0) {
                output.append(text.charAt(position + 1));
                position += 2;
                continue;
            }

            if (current == '`') {
                int run = markerRun(text, position, to, '`');
                int close = findExactRun(text, position + run, to, '`', run);
                if (close >= 0) {
                    String value = text.substring(position + run, close).replace('\n', ' ');
                    if (value.length() >= 2 && value.startsWith(" ") && value.endsWith(" ") && !value.trim().isEmpty())
                        value = value.substring(1, value.length() - 1);
                    int start = output.length();
                    output.append(value);
                    styleCode(output, start, output.length());
                    position = close + run;
                    continue;
                }
            }

            if (current == '[') {
                int labelEnd = findClosingBracket(text, position + 1, to);
                if (labelEnd >= 0 && labelEnd + 1 < to && text.charAt(labelEnd + 1) == '(') {
                    int destinationEnd = findDestinationEnd(text, labelEnd + 2, to);
                    if (destinationEnd >= 0) {
                        String destination = unescape(text.substring(labelEnd + 2, destinationEnd).trim());
                        int title = destination.indexOf(" \"");
                        if (title > 0 && destination.endsWith("\"")) destination = destination.substring(0, title);
                        int start = output.length();
                        appendInline(output, text, position + 1, labelEnd);
                        int end = output.length();
                        if (isSafeLink(destination) && end > start) {
                            output.setSpan(new URLSpan(destination), start, end, SPAN_FLAGS);
                            output.setSpan(new ForegroundColorSpan(ACCENT), start, end, SPAN_FLAGS);
                        }
                        position = destinationEnd + 1;
                        continue;
                    }
                }
            }

            String delimiter = delimiterAt(text, position, to);
            if (delimiter != null) {
                int close = findClosingDelimiter(text, position + delimiter.length(), to, delimiter);
                if (close >= 0 && close > position + delimiter.length()) {
                    int start = output.length();
                    appendInline(output, text, position + delimiter.length(), close);
                    int end = output.length();
                    applyDelimiterSpan(output, delimiter, start, end);
                    position = close + delimiter.length();
                    continue;
                }
            }

            output.append(current);
            position++;
        }
    }

    private static String delimiterAt(String text, int position, int to) {
        if (text.startsWith("***", position) && position + 3 <= to) return "***";
        if (text.startsWith("___", position) && position + 3 <= to) return "___";
        if (text.startsWith("**", position) && position + 2 <= to) return "**";
        if (text.startsWith("__", position) && position + 2 <= to && underscoreCanOpen(text, position, 2, to)) return "__";
        if (text.startsWith("~~", position) && position + 2 <= to) return "~~";
        if (text.charAt(position) == '*' && canOpen(text, position, 1, to)) return "*";
        if (text.charAt(position) == '_' && underscoreCanOpen(text, position, 1, to)) return "_";
        return null;
    }

    private static int findClosingDelimiter(String text, int from, int to, String delimiter) {
        char marker = delimiter.charAt(0);
        for (int position = from; position < to; position++) {
            if (text.charAt(position) == '\\') {
                position++;
                continue;
            }
            if (text.charAt(position) != marker) continue;
            int run = markerRun(text, position, to, marker);
            if (run >= delimiter.length()) {
                int candidate = position + run - delimiter.length();
                if (canClose(text, candidate, delimiter.length(), to)) return candidate;
            }
            position += run - 1;
        }
        return -1;
    }

    private static void applyDelimiterSpan(SpannableStringBuilder output, String delimiter, int start, int end) {
        if (end <= start) return;
        if ("~~".equals(delimiter)) output.setSpan(new StrikethroughSpan(), start, end, SPAN_FLAGS);
        else if (delimiter.length() == 3) output.setSpan(new StyleSpan(Typeface.BOLD_ITALIC), start, end, SPAN_FLAGS);
        else if (delimiter.length() == 2) output.setSpan(new StyleSpan(Typeface.BOLD), start, end, SPAN_FLAGS);
        else output.setSpan(new StyleSpan(Typeface.ITALIC), start, end, SPAN_FLAGS);
    }

    private static boolean canOpen(String text, int position, int length, int to) {
        return position + length < to && !Character.isWhitespace(text.charAt(position + length));
    }

    private static boolean underscoreCanOpen(String text, int position, int length, int to) {
        if (!canOpen(text, position, length, to)) return false;
        return position == 0 || !Character.isLetterOrDigit(text.charAt(position - 1));
    }

    private static boolean canClose(String text, int position, int length, int to) {
        if (position <= 0 || Character.isWhitespace(text.charAt(position - 1))) return false;
        if (text.charAt(position) == '_' && position + length < to
                && Character.isLetterOrDigit(text.charAt(position + length))) return false;
        return true;
    }

    private static int findClosingBracket(String text, int from, int to) {
        int depth = 0;
        for (int i = from; i < to; i++) {
            if (text.charAt(i) == '\\') { i++; continue; }
            if (text.charAt(i) == '[') depth++;
            else if (text.charAt(i) == ']') {
                if (depth == 0) return i;
                depth--;
            }
        }
        return -1;
    }

    private static int findDestinationEnd(String text, int from, int to) {
        int depth = 0;
        for (int i = from; i < to; i++) {
            if (text.charAt(i) == '\\') { i++; continue; }
            if (text.charAt(i) == '(') depth++;
            else if (text.charAt(i) == ')') {
                if (depth == 0) return i;
                depth--;
            }
        }
        return -1;
    }

    private static int findExactRun(String text, int from, int to, char marker, int length) {
        for (int i = from; i < to; i++) {
            if (text.charAt(i) != marker) continue;
            int run = markerRun(text, i, to, marker);
            if (run == length) return i;
            i += run - 1;
        }
        return -1;
    }

    private static int markerRun(String text, int position, int to, char marker) {
        int end = position;
        while (end < to && text.charAt(end) == marker) end++;
        return end - position;
    }

    private static int headingLevel(String line) {
        int spaces = leadingSpaces(line);
        if (spaces > 3) return 0;
        int count = 0;
        while (spaces + count < line.length() && count < 6 && line.charAt(spaces + count) == '#') count++;
        return count > 0 && spaces + count < line.length() && line.charAt(spaces + count) == ' ' ? count : 0;
    }

    private static boolean isHorizontalRule(String line) {
        String compact = line.trim().replace(" ", "").replace("\t", "");
        if (compact.length() < 3) return false;
        char marker = compact.charAt(0);
        if (marker != '*' && marker != '-' && marker != '_') return false;
        for (int i = 1; i < compact.length(); i++) if (compact.charAt(i) != marker) return false;
        return true;
    }

    private static int leadingSpaces(String value) {
        int count = 0;
        while (count < value.length() && value.charAt(count) == ' ') count++;
        return count;
    }

    private static int indentationWidth(String value) {
        int width = 0;
        for (int i = 0; i < value.length(); i++) width += value.charAt(i) == '\t' ? 4 : 1;
        return width;
    }

    private static boolean isSafeLink(String value) {
        String scheme = Uri.parse(value).getScheme();
        return "https".equalsIgnoreCase(scheme) || "http".equalsIgnoreCase(scheme)
                || "mailto".equalsIgnoreCase(scheme);
    }

    private static String unescape(String value) {
        StringBuilder result = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); i++) {
            if (value.charAt(i) == '\\' && i + 1 < value.length() && ESCAPABLE.indexOf(value.charAt(i + 1)) >= 0) i++;
            result.append(value.charAt(i));
        }
        return result.toString();
    }

    private static void styleCode(SpannableStringBuilder output, int start, int end) {
        if (start < 0 || end <= start) return;
        output.setSpan(new TypefaceSpan("monospace"), start, end, SPAN_FLAGS);
        output.setSpan(new BackgroundColorSpan(CODE_BACKGROUND), start, end, SPAN_FLAGS);
        output.setSpan(new ForegroundColorSpan(CODE_TEXT), start, end, SPAN_FLAGS);
    }

    private static int endWithoutTrailingNewline(SpannableStringBuilder output) {
        int length = output.length();
        return length > 0 && output.charAt(length - 1) == '\n' ? length - 1 : length;
    }

    private static int trimLastNewline(SpannableStringBuilder output) {
        int length = output.length();
        if (length > 0 && output.charAt(length - 1) == '\n') output.delete(length - 1, length);
        return output.length();
    }

    private static final class HorizontalRuleSpan extends ReplacementSpan {
        @Override public int getSize(Paint paint, CharSequence text, int start, int end, Paint.FontMetricsInt metrics) {
            if (metrics != null) {
                Paint.FontMetricsInt source = paint.getFontMetricsInt();
                metrics.ascent = source.ascent;
                metrics.descent = source.descent;
                metrics.top = source.top;
                metrics.bottom = source.bottom;
            }
            return Math.max(1, Math.round(paint.measureText("——————")));
        }

        @Override public void draw(Canvas canvas, CharSequence text, int start, int end, float x, int top,
                int y, int bottom, Paint paint) {
            int originalColor = paint.getColor();
            float originalWidth = paint.getStrokeWidth();
            paint.setColor(RULE_COLOR);
            paint.setStrokeWidth(Math.max(1f, paint.getTextSize() / 16f));
            float center = (top + bottom) / 2f;
            canvas.drawLine(x, center, x + Math.max(1f, canvas.getWidth() - x), center, paint);
            paint.setColor(originalColor);
            paint.setStrokeWidth(originalWidth);
        }
    }
}
