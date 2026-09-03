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
        String[] lines = normalize(markdown).split("\n", -1);
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

    private static final java.util.Map<String, String> SYMBOLS = createSymbols();

    private static java.util.Map<String, String> createSymbols() {
        java.util.Map<String, String> map = new java.util.HashMap<>();
        map.put("times", "\u00d7"); map.put("div", "\u00f7"); map.put("cdot", "\u00b7");
        map.put("pm", "\u00b1"); map.put("mp", "\u2213"); map.put("approx", "\u2248");
        map.put("le", "\u2264"); map.put("leq", "\u2264"); map.put("ge", "\u2265"); map.put("geq", "\u2265");
        map.put("ne", "\u2260"); map.put("neq", "\u2260"); map.put("sim", "~"); map.put("propto", "\u221d");
        map.put("infty", "\u221e"); map.put("to", "\u2192"); map.put("rightarrow", "\u2192");
        map.put("leftarrow", "\u2190"); map.put("Rightarrow", "\u21d2"); map.put("Leftarrow", "\u21d0");
        map.put("leftrightarrow", "\u2194"); map.put("sum", "\u03a3"); map.put("prod", "\u03a0");
        map.put("int", "\u222b"); map.put("sqrt", "\u221a"); map.put("ldots", "\u2026");
        map.put("dots", "\u2026"); map.put("cdots", "\u22ef"); map.put("quad", " "); map.put("qquad", "  ");
        map.put("left", ""); map.put("right", ""); map.put("text", ""); map.put("mathrm", "");
        map.put("mathbf", ""); map.put("mathit", ""); map.put("operatorname", ""); map.put("displaystyle", "");
        map.put("boxed", "");
        map.put("alpha", "\u03b1"); map.put("beta", "\u03b2"); map.put("gamma", "\u03b3");
        map.put("delta", "\u03b4"); map.put("epsilon", "\u03b5"); map.put("zeta", "\u03b6");
        map.put("eta", "\u03b7"); map.put("theta", "\u03b8"); map.put("lambda", "\u03bb");
        map.put("mu", "\u03bc"); map.put("nu", "\u03bd"); map.put("xi", "\u03be"); map.put("pi", "\u03c0");
        map.put("rho", "\u03c1"); map.put("sigma", "\u03c3"); map.put("tau", "\u03c4");
        map.put("phi", "\u03c6"); map.put("chi", "\u03c7"); map.put("psi", "\u03c8"); map.put("omega", "\u03c9");
        map.put("Gamma", "\u0393"); map.put("Delta", "\u0394"); map.put("Theta", "\u0398");
        map.put("Lambda", "\u039b"); map.put("Pi", "\u03a0"); map.put("Sigma", "\u03a3");
        map.put("Phi", "\u03a6"); map.put("Psi", "\u03a8"); map.put("Omega", "\u03a9");
        return map;
    }

    /** Applies generic LaTeX/HTML normalization outside fenced code and inline code. */
    static String normalize(String markdown) {
        String text = markdown.replace("\r\n", "\n").replace('\r', '\n');
        String[] lines = text.split("\n", -1);
        StringBuilder out = new StringBuilder(text.length());
        boolean fenced = false;
        for (int index = 0; index < lines.length; index++) {
            String line = lines[index];
            String trimmed = line.trim();
            if (trimmed.startsWith("```") || trimmed.startsWith("~~~")) { fenced = !fenced; out.append(line); }
            else if (fenced) out.append(line);
            else {
                String[] parts = line.split("`", -1);
                for (int p = 0; p < parts.length; p++) {
                    if (p > 0) out.append('`');
                    out.append(p % 2 == 0 ? stripMath(parts[p]) : parts[p]);
                }
            }
            if (index < lines.length - 1) out.append('\n');
        }
        return out.toString();
    }

    private static int matchBrace(String text, int open) {
        int depth = 0;
        for (int j = open; j < text.length(); j++) {
            char c = text.charAt(j);
            if (c == '\\') { j++; continue; }
            if (c == '{') depth++;
            else if (c == '}' && --depth == 0) return j;
        }
        return -1;
    }

    /** Generically unwraps LaTeX macros, scripts, and HTML breaks in prose text. */
    private static String stripMath(String segment) {
        String text = segment;
        for (int pass = 0; pass < 8; pass++) {
            StringBuilder out = new StringBuilder(text.length());
            int i = 0;
            while (i < text.length()) {
                char c = text.charAt(i);
                if (c == '<' && text.regionMatches(true, i, "<br", 0, 3)) {
                    int close = text.indexOf('>', i + 3);
                    if (close >= 0 && close - i <= 6) { out.append('\n'); i = close + 1; continue; }
                }
                if (c == '\\' && i + 1 < text.length()) {
                    char next = text.charAt(i + 1);
                    if (Character.isLetter(next)) {
                        int wordEnd = i + 1;
                        while (wordEnd < text.length() && Character.isLetter(text.charAt(wordEnd))) wordEnd++;
                        String name = text.substring(i + 1, wordEnd);
                        if (wordEnd < text.length() && text.charAt(wordEnd) == '{') {
                            int close = matchBrace(text, wordEnd);
                            if (close >= 0) {
                                String argument = text.substring(wordEnd + 1, close);
                                if (("frac".equals(name) || "dfrac".equals(name) || "tfrac".equals(name))
                                        && close + 1 < text.length() && text.charAt(close + 1) == '{') {
                                    int close2 = matchBrace(text, close + 1);
                                    if (close2 >= 0) {
                                        out.append(argument).append('\u2044').append(text, close + 2, close2);
                                        i = close2 + 1;
                                        continue;
                                    }
                                }
                                String symbol = SYMBOLS.get(name);
                                if (symbol != null) out.append(symbol);
                                out.append(argument);
                                i = close + 1;
                                continue;
                            }
                        }
                        String symbol = SYMBOLS.get(name);
                        if (symbol != null) { out.append(symbol); i = wordEnd; continue; }
                        out.append(text, i, wordEnd);
                        i = wordEnd;
                        continue;
                    }
                    if (next == '[' || next == ']' || next == '(' || next == ')') { i += 2; continue; }
                    if (next == ';' || next == ',' || next == ':' || next == '!') { out.append(' '); i += 2; continue; }
                }
                if ((c == '_' || c == '^') && i + 1 < text.length() && text.charAt(i + 1) == '{') {
                    int close = matchBrace(text, i + 1);
                    if (close >= 0) {
                        String argument = text.substring(i + 2, close);
                        if (argument.length() <= 3) out.append(argument);
                        else out.append(" (").append(argument).append(')');
                        i = close + 1;
                        continue;
                    }
                }
                out.append(c);
                i++;
            }
            String next = out.toString().replace("$$", "");
            if (next.equals(text)) break;
            text = next;
        }
        return text;
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
