/**
 * LogRender.h
 * The single-pass renderer that turns a log message template, its positional
 * arguments and its named fields into the sentence a sink prints.
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

#ifndef __LOGRENDER_H
#define __LOGRENDER_H

#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "LogRecord.h"

namespace gnuworld {

/**
 * One positional argument of a log statement, already detached from the
 * caller's stack: it formats itself with the spec the template gives it.
 */
using LogArg = std::function<std::string(std::string_view spec)>;

/**
 * The type a positional argument is stored as.  A string literal, a char
 * array or a char pointer becomes a std::string, so that nothing the
 * argument points at has to outlive the log statement.
 */
template <class T>
using LogArgStorage = std::conditional_t<std::is_same_v<std::decay_t<T>, char*> ||
                                             std::is_same_v<std::decay_t<T>, const char*>,
                                         std::string, std::decay_t<T>>;

/**
 * Wraps a value as a positional argument, by copy.
 * The spec is what stood after the colon in the placeholder, so that
 * "{:>5}" formats with std::format's "{:>5}" and "{}" with "{}".
 */
template <class T> LogArg makeLogArg(const T& value) {
    LogArgStorage<T> copy(value);

    return [copy = std::move(copy)](std::string_view spec) mutable -> std::string {
        std::string format;
        format.reserve(spec.size() + 3);
        format += '{';
        if (!spec.empty()) {
            format += ':';
            format += spec;
        }
        format += '}';

        return std::vformat(format, std::make_format_args(copy));
    };
}

/**
 * A rendered message, and the ranges of it that came from a substitution.
 */
struct RenderResult {
    std::string text;
    std::vector<LogSpan> spans;
};

/**
 * Renders a message template in a single pass; substituted text is never
 * rescanned.
 *
 *   "{{"       a literal '{'
 *   "}}"       a literal '}'
 *   "{}"       the next positional argument
 *   "{:spec}"  the next positional argument, formatted with "{:spec}"
 *   "{name}"   the first field whose key is name, for every occurrence;
 *              name matches [A-Za-z0-9_]+
 *
 * An unknown name, an exhausted positional, a placeholder whose spec
 * std::format rejects, an unmatched '{' and a lone '}' are all copied
 * verbatim.  A rejected spec still consumes its positional argument.
 */
RenderResult renderTemplate(std::string_view tmpl, const std::vector<LogArg>& args,
                            const std::vector<LogField>& fields);

} // namespace gnuworld

#endif // __LOGRENDER_H
