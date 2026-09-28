package app.gamenative.xrgame;

import java.util.ArrayList;

/** Convert a Wine command line to argv without passing grouping quotes to the child. */
public final class XrGameCommandLine {
    private XrGameCommandLine() {}

    public static String[] split(String command) {
        ArrayList<String> args = new ArrayList<>();
        StringBuilder arg = new StringBuilder();
        char quote = 0;
        boolean started = false;
        for (int i = 0; i < command.length(); i++) {
            char c = command.charAt(i);
            if (c == '\\' && quote != '\'') {
                int start = i;
                while (i < command.length() && command.charAt(i) == '\\') i++;
                int count = i - start;
                if (i < command.length() && command.charAt(i) == '"') {
                    for (int n = 0; n < count / 2; n++) arg.append('\\');
                    if (count % 2 != 0) arg.append('"');
                    else quote = quote == '"' ? 0 : '"';
                } else {
                    for (int n = 0; n < count; n++) arg.append('\\');
                    i--;
                }
                started = true;
            } else if (c == quote) {
                quote = 0;
            } else if (quote == 0 && (c == '"' || c == '\'')) {
                quote = c;
                started = true;
            } else if (quote == 0 && Character.isWhitespace(c)) {
                if (started) {
                    args.add(arg.toString());
                    arg.setLength(0);
                    started = false;
                }
            } else {
                arg.append(c);
                started = true;
            }
        }
        if (quote != 0) throw new IllegalArgumentException("Unclosed quote in Wine command line");
        if (started) args.add(arg.toString());
        return args.toArray(new String[0]);
    }
}
