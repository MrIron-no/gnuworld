/**
 * LogRender.cc
 * The single-pass message template renderer.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 *
 */

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "LogRecord.h"
#include "LogRender.h"

namespace gnuworld {

namespace {

/// True for the characters a named placeholder is made of: [A-Za-z0-9_]
bool isNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || '_' == c;
}

/// True when body is a name, so that "{body}" is a field reference
bool isName(std::string_view body) {
    if (body.empty())
        return false;
    for (const char c : body)
        if (!isNameChar(c))
            return false;
    return true;
}

/// The first field with this key, or nullptr.  A display-only field counts:
/// it is there for the template and left out of JSON, not the other way
/// round.
const LogField* findField(const std::vector<LogField>& fields, std::string_view key) {
    for (const LogField& field : fields)
        if (field.key == key)
            return &field;
    return nullptr;
}

} // namespace

RenderResult renderTemplate(std::string_view tmpl, const std::vector<LogArg>& args,
                            const std::vector<LogField>& fields) {
    RenderResult result;
    result.text.reserve(tmpl.size());

    /// Appends a substitution and records the range it occupies
    const auto substitute = [&result](const std::string& value) {
        const std::size_t begin = result.text.size();
        result.text += value;
        result.spans.push_back(LogSpan{begin, result.text.size()});
    };

    std::size_t nextArg = 0;
    std::size_t pos = 0;

    while (pos < tmpl.size()) {
        const char c = tmpl[pos];

        if ('}' == c) {
            // "}}" is a literal '}'; a lone '}' is copied verbatim
            if (pos + 1 < tmpl.size() && '}' == tmpl[pos + 1])
                ++pos;
            result.text += '}';
            ++pos;
            continue;
        }

        if ('{' != c) {
            result.text += c;
            ++pos;
            continue;
        }

        // "{{" is a literal '{'
        if (pos + 1 < tmpl.size() && '{' == tmpl[pos + 1]) {
            result.text += '{';
            pos += 2;
            continue;
        }

        // Look for the '}' that closes this placeholder.  Another '{' first
        // means this one opens nothing, so it is copied verbatim and the
        // scan carries on at the one after it.
        std::size_t close = pos + 1;
        while (close < tmpl.size() && '}' != tmpl[close] && '{' != tmpl[close])
            ++close;

        if (close >= tmpl.size() || '{' == tmpl[close]) {
            result.text += '{';
            ++pos;
            continue;
        }

        const std::string_view body = tmpl.substr(pos + 1, close - pos - 1);
        const std::string_view placeholder = tmpl.substr(pos, close - pos + 1);

        if (body.empty() || ':' == body.front()) {
            // A positional argument, with the spec that follows the colon
            if (nextArg >= args.size()) {
                result.text += placeholder;
                pos = close + 1;
                continue;
            }

            const LogArg& arg = args[nextArg++];
            const std::string_view spec = body.empty() ? std::string_view() : body.substr(1);

            try {
                substitute(arg(spec));
            } catch (const std::format_error&) {
                // A spec std::format will not have: the placeholder stays as
                // it was written, but the argument is spent
                result.text += placeholder;
            }

            pos = close + 1;
            continue;
        }

        if (isName(body)) {
            const LogField* const field = findField(fields, body);
            if (field != nullptr)
                substitute(logValueToString(field->value));
            else
                result.text += placeholder; // an unknown name
            pos = close + 1;
            continue;
        }

        // Neither a positional nor a name: nothing to substitute
        result.text += placeholder;
        pos = close + 1;
    }

    return result;
}

} // namespace gnuworld
