# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The extractor behind tests/static/check_aspace_sigdiff.sh. Prints one TAB-separated
# record per address-space seam member a header declares. Input must be ALREADY-STRIPPED
# (tests/lib/strip_comments.awk), one file at a time:
#   awk -f aspace_seam.awk <stripped-file>
#
#   <kind> <TAB> <name> <TAB> <detail> <TAB> <preprocessor guard>
#
# Records carry no file and no line number, so moving a member between seam headers or
# reordering the declarations in one is not a difference.
#
# MEMBERSHIP IS BY DECLARED IDENTIFIER, never by the section banner in the header: a
# re-wrapped or deleted comment would shrink a banner-keyed corpus to nothing and the
# caller would report an empty diff, which is the false PASS this instrument exists to
# rule out. An identifier matching PREFIX below is a member wherever it stands.
#
# kinds:
#   TAG         a struct, enum or union tag, with complete or incomplete
#   ENUMERATOR  one enumeration constant and its VALUE, evaluated where the expression is
#               integer literals joined by `<<`, `|`, `+` or `-`, and carried as
#               `expr:<text>` where it is not
#   TYPEDEF     an alias and its underlying type
#   MACRO       an object-like macro and its value, evaluated as above
#   MACROFN     a function-like macro, its parameter list and its body
#   FUNC        a function: return type and parameter TYPES, names dropped
#   FUNCNAMES   the same function with parameter names KEPT, for the caller's
#               non-signature section
#   OBJ         an object declaration and its type
#
# Exits 2 with a REFUSE line on stderr when the file cannot be walked, so an unparsable
# header is never reported as clean.
function trim(s) {
    sub(/^[ \t]+/, "", s)
    sub(/[ \t]+$/, "", s)
    return s
}
# Whitespace collapsed and `*` made a token of its own, so a hand re-wrap of a parameter
# list is not a difference. A pointer star must NOT glue to the identifier beside it: that
# fuses the type and the parameter name into one token and the name-dropping below stops
# firing, which turns a rename into a reported signature change.
function canon(s) {
    gsub(/[ \t]+/, " ", s)
    gsub(/[ ]*\*[ ]*/, " * ", s)
    gsub(/[ ]*,[ ]*/, ", ", s)
    gsub(/[ ]+/, " ", s)
    return trim(s)
}
function hexval(h,    i, n, c, v, d) {
    v = 0
    n = length(h)
    for (i = 1; i <= n; i++) {
        c = substr(h, i, 1)
        d = index("0123456789abcdef", tolower(c))
        if (d == 0) { return "" }
        v = v * 16 + (d - 1)
    }
    return v
}
# An integer literal with its suffix and any wrapping parentheses removed, or "".
function litval(x,    neg) {
    x = trim(x)
    while (x ~ /^\(.*\)$/) {
        x = trim(substr(x, 2, length(x) - 2))
    }
    sub(/[uUlL]+$/, "", x)
    neg = 0
    if (x ~ /^-/) {
        neg = 1
        x = substr(x, 2)
    }
    if (x ~ /^0[xX][0-9a-fA-F]+$/) {
        x = hexval(substr(x, 3))
    } else if (x ~ /^[0-9]+$/) {
        x = x + 0
    } else {
        return ""
    }
    if (x == "") { return "" }
    if (neg) { return -x }
    return x
}
function bor(a, b,    r, bit) {
    r = 0
    bit = 1
    while (a > 0 || b > 0) {
        if ((a % 2) == 1 || (b % 2) == 1) { r = r + bit }
        a = int(a / 2)
        b = int(b / 2)
        bit = bit * 2
    }
    return r
}
# One `<<` chain of literals, or "".
function evalshift(s,    n, p, i, v, sh, t) {
    n = split(s, p, "<<")
    v = litval(p[1])
    if (v == "") { return "" }
    for (i = 2; i <= n; i++) {
        sh = litval(p[i])
        if (sh == "") { return "" }
        t = 0
        while (t < sh) {
            v = v * 2
            t = t + 1
        }
    }
    return v
}
# One `+`/`-` chain of `<<` chains, or "".
function evalsum(s,    n, p, i, v, t, sign, rest) {
    gsub(/-/, "+-", s)
    sub(/^\+/, "", s)
    n = split(s, p, "+")
    v = 0
    for (i = 1; i <= n; i++) {
        t = p[i]
        if (t == "") { return "" }
        sign = 1
        if (substr(t, 1, 1) == "-") {
            sign = -1
            t = substr(t, 2)
            if (t ~ /^[0-9]/) {
                t = "-" t
                sign = 1
            }
        }
        rest = evalshift(t)
        if (rest == "") { return "" }
        v = v + sign * rest
    }
    return v
}
# The value of a constant expression, or `expr:<canonical text>` when it is not one this
# evaluator covers. A textual rewrite that leaves the value alone is therefore silent,
# and an unevaluable expression is reported as text rather than assumed equal.
function evalval(t,    s, n, p, i, v, acc) {
    s = t
    gsub(/[ \t]/, "", s)
    if (s == "") { return "(empty)" }
    while (s ~ /^\(.*\)$/ && index(substr(s, 2, length(s) - 2), "(") == 0) {
        s = substr(s, 2, length(s) - 2)
    }
    n = split(s, p, "|")
    acc = 0
    for (i = 1; i <= n; i++) {
        v = evalsum(p[i])
        if (v == "" || v < 0) { return "expr:" canon(t) }
        acc = bor(acc, v)
    }
    return acc ""
}
function is_member(nm) {
    if (nm ~ PREFIX) { return 1 }
    return 0
}
function emit(kind, nm, detail) {
    printf("%s\t%s\t%s\t%s\n", kind, nm, detail, guard())
}
function guard(    i, s) {
    s = ""
    for (i = 1; i <= gdepth; i++) {
        if (gkind[i] == "skip") { continue }
        if (s != "") { s = s " && " }
        s = s gstack[i]
    }
    if (s == "") { return "-" }
    return s
}
# The last identifier of a declarator, which is the declared name.
function tail_ident(s,    n, p, i, t) {
    gsub(/[^A-Za-z0-9_]/, " ", s)
    n = split(s, p, " ")
    for (i = n; i >= 1; i--) {
        t = p[i]
        if (t != "") { return t }
    }
    return ""
}
# One parameter with its name dropped. The name is the trailing token when the parameter
# has two or more tokens and that token is neither a type keyword, nor `_t`-suffixed, nor
# pointer or array punctuation. Nothing is dropped when the remainder would be empty, so
# a type token can never be eaten and read as unchanged.
function strip_pname(p,    n, tk, i, last, out) {
    p = canon(p)
    if (p == "") { return "" }
    n = split(p, tk, " ")
    if (n < 2) { return p }
    last = tk[n]
    if (last ~ TYPEKW) { return p }
    if (last ~ /_t$/) { return p }
    if (last ~ /[][*()]/) { return p }
    out = ""
    for (i = 1; i < n; i++) {
        if (out != "") { out = out " " }
        out = out tk[i]
    }
    if (trim(out) == "") { return p }
    return trim(out)
}
function params_notypes(plist,    n, p, i, out, one) {
    n = split(plist, p, ",")
    out = ""
    for (i = 1; i <= n; i++) {
        one = strip_pname(p[i])
        if (out != "") { out = out ", " }
        out = out one
    }
    return out
}
# The enumerator list of a captured enum body.
function do_enum(stmt,    ob, cb, head, body, tag, n, p, i, one, nm, val, next_ord) {
    ob = index(stmt, "{")
    cb = length(stmt)
    while (cb > 0 && substr(stmt, cb, 1) != "}") { cb-- }
    if (ob == 0 || cb <= ob) {
        refuse = "enum body brackets not found"
        exit 2
    }
    head = canon(substr(stmt, 1, ob - 1))
    body = substr(stmt, ob + 1, cb - ob - 1)
    tag = ""
    if (match(head, /(struct|union|enum)[ ]+[A-Za-z_][A-Za-z0-9_]*/)) {
        tag = tail_ident(substr(head, RSTART, RLENGTH))
    }
    if (head ~ /(^|[^A-Za-z0-9_])enum([^A-Za-z0-9_]|$)/) {
        if (tag != "" && is_member(tag)) { emit("TAG", tag, "enum complete") }
        n = split(body, p, ",")
        next_ord = 0
        for (i = 1; i <= n; i++) {
            one = trim(p[i])
            if (one == "") { continue }
            if (!match(one, /^[A-Za-z_][A-Za-z0-9_]*/)) { continue }
            nm = substr(one, RSTART, RLENGTH)
            val = trim(substr(one, RLENGTH + 1))
            if (substr(val, 1, 1) == "=") {
                val = evalval(substr(val, 2))
            } else {
                val = next_ord ""
            }
            if (val ~ /^-?[0-9]+$/) { next_ord = val + 1 }
            if (is_member(nm) || (tag != "" && is_member(tag))) {
                emit("ENUMERATOR", nm, "= " val)
            }
        }
        return
    }
    # A complete struct or union: the tag and its member types, in order.
    if (tag != "" && is_member(tag)) {
        emit("TAG", tag, "struct complete " canon(body))
    }
}
# A declaration ended by `;` at outer nesting.
function flush(stmt,    s, nm, ob, op, cp, d, i, n, c, head, plist, ret) {
    s = canon(stmt)
    if (s == "") { return }
    if (index(s, "{") > 0) {
        do_enum(stmt)
        return
    }
    if (s ~ /^(typedef)([^A-Za-z0-9_]|$)/) {
        sub(/^typedef[ ]+/, "", s)
        nm = tail_ident(s)
        if (!is_member(nm)) { return }
        # A function-pointer or array alias is carried verbatim rather than split.
        if (index(s, "(") > 0 || index(s, "[") > 0) {
            emit("TYPEDEF", nm, "verbatim:" s)
            return
        }
        sub(/[ ]*[A-Za-z_][A-Za-z0-9_]*$/, "", s)
        emit("TYPEDEF", nm, canon(s))
        return
    }
    if (s ~ /^(struct|union|enum)[ ]+[A-Za-z_][A-Za-z0-9_]*$/) {
        nm = tail_ident(s)
        if (!is_member(nm)) { return }
        c = "struct"
        if (s ~ /^union/) { c = "union" }
        if (s ~ /^enum/) { c = "enum" }
        emit("TAG", nm, c " incomplete")
        return
    }
    # The first parenthesis at outer nesting opens the parameter list.
    op = 0
    d = 0
    n = length(s)
    for (i = 1; i <= n; i++) {
        c = substr(s, i, 1)
        if (c == "(") {
            op = i
            break
        }
    }
    if (op == 0) {
        nm = tail_ident(s)
        if (!is_member(nm)) { return }
        sub(/[ ]*[A-Za-z_][A-Za-z0-9_]*$/, "", s)
        emit("OBJ", nm, canon(s))
        return
    }
    cp = 0
    d = 0
    for (i = op; i <= n; i++) {
        c = substr(s, i, 1)
        if (c == "(") { d++ }
        if (c == ")") {
            d--
            if (d == 0) {
                cp = i
                break
            }
        }
    }
    if (cp == 0) {
        refuse = "unbalanced parenthesis in a declaration"
        exit 2
    }
    head = trim(substr(s, 1, op - 1))
    plist = substr(s, op + 1, cp - op - 1)
    nm = tail_ident(head)
    if (!is_member(nm)) { return }
    # A declarator this splitter does not model is carried verbatim, never as a signature
    # it guessed at.
    if (head ~ /[*][ ]*$/ || index(head, "(") > 0 || index(head, "[") > 0 || substr(s, cp + 1) ~ /[^ ]/) {
        emit("FUNC", nm, "verbatim:" s)
        emit("FUNCNAMES", nm, "verbatim:" s)
        return
    }
    ret = trim(substr(head, 1, length(head) - length(nm)))
    ret = canon(ret)
    if (ret == "") { ret = "(implicit-int)" }
    emit("FUNC", nm, ret " (" params_notypes(plist) ")")
    emit("FUNCNAMES", nm, ret " (" canon(plist) ")")
}
# The declarator that opened a definition body, closed by `}` at outer nesting.
function flush_def(pre,    s, nm) {
    s = canon(pre)
    if (s == "") { return }
    if (index(s, "(") == 0) { return }
    flush(s)
}
BEGIN {
    PREFIX = "^(arch_aspace|ARCH_ASPACE|arch_map|ARCH_MAP|arch_phys_addr)"
    TYPEKW = "^(void|char|short|int|long|float|double|signed|unsigned|bool|_Bool|const|volatile|restrict|struct|enum|union|__restrict)$"
    depth = 0
    xdepth = 0
    capture = 0
    stmt = ""
    defpre = ""
    gdepth = 0
    pending_ig = ""
    refuse = ""
}
/^[ \t]*#/ {
    d = $0
    sub(/^[ \t]*#[ \t]*/, "", d)
    if (d ~ /^define([ \t]|$)/) {
        sub(/^define[ \t]*/, "", d)
        if (match(d, /^[A-Za-z_][A-Za-z0-9_]*/)) {
            nm = substr(d, RSTART, RLENGTH)
            rest = substr(d, RLENGTH + 1)
            if (nm == pending_ig && gdepth > 0) { gkind[gdepth] = "skip" }
            if (is_member(nm)) {
                if (substr(rest, 1, 1) == "(") {
                    cp = index(rest, ")")
                    if (cp == 0) {
                        refuse = "function-like macro with no closing parenthesis at line " FNR
                        exit 2
                    }
                    emit("MACROFN", nm,
                         "(" canon(substr(rest, 2, cp - 2)) ") " canon(substr(rest, cp + 1)))
                } else {
                    emit("MACRO", nm, "= " evalval(rest))
                }
            }
        }
        pending_ig = ""
        next
    }
    pending_ig = ""
    if (d ~ /^ifndef([ \t]|$)/) {
        sub(/^ifndef[ \t]*/, "", d)
        d = trim(d)
        gdepth++
        gstack[gdepth] = "!defined(" d ")"
        gkind[gdepth] = "if"
        pending_ig = d
        next
    }
    if (d ~ /^ifdef([ \t]|$)/) {
        sub(/^ifdef[ \t]*/, "", d)
        gdepth++
        gstack[gdepth] = "defined(" trim(d) ")"
        gkind[gdepth] = "if"
        next
    }
    if (d ~ /^if([ \t]|$)/) {
        sub(/^if[ \t]*/, "", d)
        gdepth++
        gstack[gdepth] = trim(d)
        gkind[gdepth] = "if"
        next
    }
    if (d ~ /^elif([ \t]|$)/) {
        sub(/^elif[ \t]*/, "", d)
        if (gdepth == 0) {
            refuse = "#elif with no open conditional at line " FNR
            exit 2
        }
        gstack[gdepth] = "!(" gstack[gdepth] ") && (" trim(d) ")"
        gkind[gdepth] = "if"
        next
    }
    if (d ~ /^else([ \t]|$)/) {
        if (gdepth == 0) {
            refuse = "#else with no open conditional at line " FNR
            exit 2
        }
        if (gkind[gdepth] != "skip") { gstack[gdepth] = "!(" gstack[gdepth] ")" }
        next
    }
    if (d ~ /^endif([ \t]|$)/) {
        if (gdepth == 0) {
            refuse = "#endif with no open conditional at line " FNR
            exit 2
        }
        gdepth--
        next
    }
    next
}
{
    line = $0
    if (line ~ /[^ \t]/) { pending_ig = "" }
    i = 1
    n = length(line)
    while (i <= n) {
        c = substr(line, i, 1)
        if (c == "{") {
            if (depth == 0) {
                pre = trim(stmt)
                # An `extern "C"` block is transparent: strip_comments.awk removes the
                # literal, so the opener reduces to the bare keyword. Counting its brace
                # would put every seam declaration at nesting 1 and skip all of them.
                if (pre ~ /^extern[ \t]*$/) {
                    xdepth++
                    stmt = ""
                    i++
                    continue
                }
                if (pre ~ /(^|[^A-Za-z0-9_])(enum|struct|union)([^A-Za-z0-9_]|$)/) {
                    capture = 1
                } else {
                    capture = 0
                    defpre = pre
                    stmt = ""
                }
            }
            depth++
            if (capture) { stmt = stmt c }
            i++
            continue
        }
        if (c == "}") {
            if (depth == 0) {
                if (xdepth > 0) {
                    xdepth--
                    stmt = ""
                    i++
                    continue
                }
                refuse = "unmatched closing brace at line " FNR
                exit 2
            }
            depth--
            if (capture) {
                stmt = stmt c
            } else if (depth == 0) {
                flush_def(defpre)
                defpre = ""
                stmt = ""
            }
            i++
            continue
        }
        if (c == ";" && depth == 0) {
            flush(stmt)
            stmt = ""
            capture = 0
            i++
            continue
        }
        if (depth == 0 || capture) { stmt = stmt c }
        i++
    }
    stmt = stmt " "
}
END {
    if (refuse != "") {
        print "REFUSE " FILENAME ": " refuse > "/dev/stderr"
        exit 2
    }
    if (depth != 0 || xdepth != 0) {
        print "REFUSE " FILENAME ": braces do not balance (nesting " depth ", extern \"C\" " xdepth ")" > "/dev/stderr"
        exit 2
    }
    if (gdepth != 0) {
        print "REFUSE " FILENAME ": " gdepth " preprocessor conditional(s) left open" > "/dev/stderr"
        exit 2
    }
    if (trim(stmt) != "") {
        print "REFUSE " FILENAME ": trailing text with no semicolon: " trim(stmt) > "/dev/stderr"
        exit 2
    }
}
