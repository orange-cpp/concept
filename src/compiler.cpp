#include "concept/compiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cpt {
namespace {

enum class TokenKind {
    end,
    identifier,
    integer,
    floating,
    string_literal,
    import_kw,
    class_kw,
    fn_kw,
    type_kw,
    return_kw,
    if_kw,
    else_kw,
    while_kw,
    true_kw,
    false_kw,
    at,
    left_paren,
    right_paren,
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    semicolon,
    comma,
    dot,
    arrow,
    assign,
    plus,
    minus,
    star,
    slash,
    percent,
    ampersand,
    bang,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

struct Token {
    TokenKind kind{};
    std::string text;
    std::size_t line{};
    std::size_t column{};
    std::string filename;
};

ValueType value_type_from_name(const std::string_view name) {
    if (name == "bool") {
        return ValueType::boolean;
    }
    if (name == "i8") {
        return ValueType::i8;
    }
    if (name == "i16") {
        return ValueType::i16;
    }
    if (name == "i32" || name == "int") {
        return ValueType::i32;
    }
    if (name == "i64") {
        return ValueType::i64;
    }
    if (name == "u8") {
        return ValueType::u8;
    }
    if (name == "u16") {
        return ValueType::u16;
    }
    if (name == "u32") {
        return ValueType::u32;
    }
    if (name == "u64") {
        return ValueType::u64;
    }
    if (name == "f32" || name == "float") {
        return ValueType::f32;
    }
    if (name == "f64" || name == "double") {
        return ValueType::f64;
    }
    if (name == "string" || name == "text") {
        return ValueType::text;
    }
    throw std::logic_error("invalid type token");
}

std::string_view value_type_name(const ValueType type) {
    switch (type) {
    case ValueType::boolean:
        return "bool";
    case ValueType::i8:
        return "i8";
    case ValueType::i16:
        return "i16";
    case ValueType::i32:
        return "i32";
    case ValueType::i64:
        return "i64";
    case ValueType::u8:
        return "u8";
    case ValueType::u16:
        return "u16";
    case ValueType::u32:
        return "u32";
    case ValueType::u64:
        return "u64";
    case ValueType::f32:
        return "f32";
    case ValueType::f64:
        return "f64";
    case ValueType::text:
        return "string";
    }
    throw std::logic_error("invalid value type");
}

bool is_type_name(const std::string_view name) {
    return name == "bool" || name == "i8" || name == "i16" ||
           name == "i32" || name == "i64" || name == "u8" ||
           name == "u16" || name == "u32" || name == "u64" ||
           name == "f32" || name == "f64" || name == "int" ||
           name == "float" || name == "double" || name == "string" ||
           name == "text";
}

std::uint64_t mix_seed(std::uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t fresh_opcode_seed() {
    static const std::uint64_t process_seed = [] {
        std::random_device random;
        auto seed = static_cast<std::uint64_t>(random());
        seed = (seed << 32) ^ static_cast<std::uint64_t>(random());
        seed ^= static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count());
        return mix_seed(seed);
    }();
    static std::atomic<std::uint64_t> sequence{};
    return mix_seed(process_seed +
                    sequence.fetch_add(1, std::memory_order_relaxed));
}

template <std::size_t Size>
void fill_random_bytes(std::array<std::uint8_t, Size>& output) {
    for (std::size_t offset = 0; offset < output.size(); offset += 8) {
        const auto value = fresh_opcode_seed();
        const auto count = std::min<std::size_t>(8, output.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            output[offset + index] =
                static_cast<std::uint8_t>(value >> (index * 8));
        }
    }
}

class Lexer {
public:
    Lexer(const std::string_view source, const std::string_view filename)
        : source_(source), filename_(filename) {}

    Token next() {
        skip_ignored();
        if (at_end()) {
            return make_token(TokenKind::end, position_, 0, line_, column_);
        }

        const auto start = position_;
        const auto line = line_;
        const auto column = column_;
        const char character = advance();

        if (character == '"') {
            while (!at_end() && peek() != '"') {
                if (peek() == '\n' || peek() == '\r') {
                    fail(line, column, "unterminated string literal");
                }
                if (advance() == '\\') {
                    if (at_end()) {
                        fail(line, column, "unterminated string escape");
                    }
                    advance();
                }
            }
            if (at_end()) {
                fail(line, column, "unterminated string literal");
            }
            advance();
            return make_token(TokenKind::string_literal, start + 1,
                              position_ - start - 2, line, column);
        }

        if (is_identifier_start(character)) {
            for (;;) {
                while (!at_end() && is_identifier_part(peek())) {
                    advance();
                }
                if (position_ + 2 >= source_.size() ||
                    source_[position_] != ':' ||
                    source_[position_ + 1] != ':' ||
                    !is_identifier_start(source_[position_ + 2])) {
                    break;
                }
                advance();
                advance();
            }
            const auto text = source_.substr(start, position_ - start);
            TokenKind kind = TokenKind::identifier;
            if (text == "import") {
                kind = TokenKind::import_kw;
            } else if (text == "class") {
                kind = TokenKind::class_kw;
            } else if (text == "fn") {
                kind = TokenKind::fn_kw;
            } else if (is_type_name(text)) {
                kind = TokenKind::type_kw;
            } else if (text == "return") {
                kind = TokenKind::return_kw;
            } else if (text == "if") {
                kind = TokenKind::if_kw;
            } else if (text == "else") {
                kind = TokenKind::else_kw;
            } else if (text == "while") {
                kind = TokenKind::while_kw;
            } else if (text == "true") {
                kind = TokenKind::true_kw;
            } else if (text == "false") {
                kind = TokenKind::false_kw;
            }
            return make_token(kind, start, position_ - start, line, column);
        }

        if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
            if (character == '0' && !at_end() &&
                (peek() == 'x' || peek() == 'X')) {
                advance();
                const auto digits_begin = position_;
                while (!at_end() &&
                       std::isxdigit(static_cast<unsigned char>(peek())) != 0) {
                    advance();
                }
                if (position_ == digits_begin) {
                    fail(line, column,
                         "hexadecimal literal requires at least one digit");
                }
                return make_token(TokenKind::integer, start,
                                  position_ - start, line, column);
            }
            while (!at_end() &&
                   std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
            bool is_floating_literal = false;
            if (!at_end() && peek() == '.') {
                is_floating_literal = true;
                advance();
                while (!at_end() &&
                       std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                    advance();
                }
            }
            if (!at_end() && (peek() == 'e' || peek() == 'E')) {
                is_floating_literal = true;
                advance();
                if (!at_end() && (peek() == '+' || peek() == '-')) {
                    advance();
                }
                if (at_end() ||
                    std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                    fail(line, column, "invalid floating-point exponent");
                }
                while (!at_end() &&
                       std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                    advance();
                }
            }
            return make_token(is_floating_literal ? TokenKind::floating
                                                   : TokenKind::integer,
                              start, position_ - start, line, column);
        }

        switch (character) {
        case '@':
            return make_token(TokenKind::at, start, 1, line, column);
        case '(':
            return make_token(TokenKind::left_paren, start, 1, line, column);
        case ')':
            return make_token(TokenKind::right_paren, start, 1, line, column);
        case '{':
            return make_token(TokenKind::left_brace, start, 1, line, column);
        case '}':
            return make_token(TokenKind::right_brace, start, 1, line, column);
        case '[':
            return make_token(TokenKind::left_bracket, start, 1, line, column);
        case ']':
            return make_token(TokenKind::right_bracket, start, 1, line, column);
        case ';':
            return make_token(TokenKind::semicolon, start, 1, line, column);
        case ',':
            return make_token(TokenKind::comma, start, 1, line, column);
        case '.':
            return make_token(TokenKind::dot, start, 1, line, column);
        case '+':
            return make_token(TokenKind::plus, start, 1, line, column);
        case '*':
            return make_token(TokenKind::star, start, 1, line, column);
        case '/':
            return make_token(TokenKind::slash, start, 1, line, column);
        case '%':
            return make_token(TokenKind::percent, start, 1, line, column);
        case '&':
            return make_token(TokenKind::ampersand, start, 1, line, column);
        case '-':
            if (match('>')) {
                return make_token(TokenKind::arrow, start, 2, line, column);
            }
            return make_token(TokenKind::minus, start, 1, line, column);
        case '=':
            if (match('=')) {
                return make_token(TokenKind::equal, start, 2, line, column);
            }
            return make_token(TokenKind::assign, start, 1, line, column);
        case '!':
            if (match('=')) {
                return make_token(TokenKind::not_equal, start, 2, line, column);
            }
            return make_token(TokenKind::bang, start, 1, line, column);
        case '<':
            if (match('=')) {
                return make_token(TokenKind::less_equal, start, 2, line, column);
            }
            return make_token(TokenKind::less, start, 1, line, column);
        case '>':
            if (match('=')) {
                return make_token(TokenKind::greater_equal, start, 2, line,
                                  column);
            }
            return make_token(TokenKind::greater, start, 1, line, column);
        default:
            fail(line, column,
                 "unexpected character '" + std::string(1, character) + "'");
        }
    }

private:
    std::string_view source_;
    std::string_view filename_;
    std::size_t position_{};
    std::size_t line_{1};
    std::size_t column_{1};

    [[nodiscard]] bool at_end() const { return position_ >= source_.size(); }
    [[nodiscard]] char peek() const { return source_[position_]; }

    char advance() {
        const char character = source_[position_++];
        if (character == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return character;
    }

    bool match(const char expected) {
        if (at_end() || peek() != expected) {
            return false;
        }
        advance();
        return true;
    }

    void skip_ignored() {
        for (;;) {
            while (!at_end() &&
                   std::isspace(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
            if (position_ + 1 < source_.size() && source_[position_] == '/' &&
                source_[position_ + 1] == '/') {
                while (!at_end() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            return;
        }
    }

    static bool is_identifier_start(const char character) {
        return std::isalpha(static_cast<unsigned char>(character)) != 0 ||
               character == '_';
    }

    static bool is_identifier_part(const char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
               character == '_';
    }

    [[nodiscard]] Token make_token(const TokenKind kind,
                                   const std::size_t start,
                                   const std::size_t length,
                                   const std::size_t line,
                                   const std::size_t column) const {
        return {kind, std::string(source_.substr(start, length)), line, column,
                std::string(filename_)};
    }

    [[noreturn]] void fail(const std::size_t line, const std::size_t column,
                           const std::string& message) const {
        throw CompileError(std::string(filename_) + ':' + std::to_string(line) +
                           ':' + std::to_string(column) + ": " + message);
    }
};

struct Expr {
    enum class Kind {
        literal,
        variable,
        member,
        call,
        cast,
        pointer_cast,
        index,
        unary,
        binary,
    } kind{};
    Token token;
    std::uint64_t bits{};
    ValueType value_type{ValueType::i64};
    std::string name;
    std::string text;
    TokenKind op{};
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct Stmt {
    enum class Kind {
        block,
        variable,
        assign,
        field_assign,
        indirect_assign,
        expression,
        return_value,
        if_statement,
        while_statement,
    } kind{};
    Token token;
    std::string name;
    ValueType value_type{ValueType::i64};
    bool template_type{};
    std::uint8_t pointer_depth{};
    std::uint32_t array_length{};
    std::string class_name;
    std::unique_ptr<Expr> target;
    std::unique_ptr<Expr> expression;
    std::unique_ptr<Stmt> first;
    std::unique_ptr<Stmt> second;
    std::vector<std::unique_ptr<Stmt>> statements;
};

struct Function {
    Token name;
    ValueType return_type{ValueType::i64};
    bool return_template_type{};
    std::uint8_t complexity{};
    std::string class_name;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct Field {
    Token name;
    ValueType type{ValueType::i64};
    bool template_type{};
    std::uint8_t pointer_depth{};
};

struct Class {
    Token name;
    std::string template_parameter;
    std::vector<Field> fields;
    std::vector<Function> methods;
};

struct Import {
    Token name;
};

struct ParsedModule {
    std::vector<Import> imports;
    std::vector<Class> classes;
    std::vector<Function> functions;
};

class Parser {
public:
    Parser(const std::string_view source, const std::string_view filename)
        : lexer_(source, filename), current_(lexer_.next()),
          next_(lexer_.next()), after_next_(lexer_.next()),
          following_(lexer_.next()) {}

    ParsedModule parse_program() {
        ParsedModule module;
        while (current_.kind != TokenKind::end) {
            if (match(TokenKind::import_kw)) {
                const auto name =
                    consume(TokenKind::identifier, "expected module name");
                consume(TokenKind::semicolon,
                        "expected ';' after import name");
                module.imports.push_back({name});
            } else if (current_.kind == TokenKind::class_kw) {
                module.classes.push_back(parse_class());
            } else {
                module.functions.push_back(parse_function());
            }
        }
        return module;
    }

private:
    Lexer lexer_;
    Token current_;
    Token next_;
    Token after_next_;
    Token following_;
    std::string active_template_parameter_;

    void advance() {
        current_ = next_;
        next_ = after_next_;
        after_next_ = following_;
        following_ = lexer_.next();
    }

    bool match(const TokenKind kind) {
        if (current_.kind != kind) {
            return false;
        }
        advance();
        return true;
    }

    Token consume(const TokenKind kind, const std::string_view message) {
        if (current_.kind != kind) {
            fail(current_, message);
        }
        const Token token = current_;
        advance();
        return token;
    }

    [[noreturn]] void fail(const Token& token,
                           const std::string_view message) const {
        throw CompileError(token.filename + ':' +
                           std::to_string(token.line) + ':' +
                           std::to_string(token.column) + ": " +
                           std::string(message));
    }

    Function parse_function(const std::string_view class_name = {}) {
        Function function;
        function.class_name = class_name;
        bool has_complexity = false;
        while (match(TokenKind::at)) {
            const auto decorator =
                consume(TokenKind::identifier, "expected decorator name");
            if (decorator.text != "complexity" &&
                decorator.text != "complexty") {
                fail(decorator, "unknown decorator '@" +
                                    std::string(decorator.text) + "'");
            }
            if (has_complexity) {
                fail(decorator, "duplicate complexity decorator");
            }
            has_complexity = true;
            consume(TokenKind::left_paren,
                    "expected '(' after complexity decorator");
            const auto value = consume(
                TokenKind::integer,
                "expected an integer from 0 to 100 for complexity");
            std::uint32_t parsed = 0;
            const auto result = std::from_chars(
                value.text.data(), value.text.data() + value.text.size(),
                parsed);
            if (result.ec != std::errc{} ||
                result.ptr != value.text.data() + value.text.size() ||
                parsed > 100) {
                fail(value, "complexity must be between 0 and 100");
            }
            function.complexity = static_cast<std::uint8_t>(parsed);
            consume(TokenKind::right_paren,
                    "expected ')' after complexity value");
        }

        consume(TokenKind::fn_kw, "expected 'fn'");
        function.name =
            consume(TokenKind::identifier, "expected function name");
        consume(TokenKind::left_paren, "expected '('");
        consume(TokenKind::right_paren,
                "function parameters are not supported yet; expected ')'");
        consume(TokenKind::arrow, "expected '->'");
        if (!active_template_parameter_.empty() &&
            current_.kind == TokenKind::identifier &&
            current_.text == active_template_parameter_) {
            function.return_template_type = true;
            advance();
        } else {
            const auto return_type =
                consume(TokenKind::type_kw, "expected function return type");
            function.return_type = value_type_from_name(return_type.text);
        }
        function.body = parse_block_contents();
        return function;
    }

    Class parse_class() {
        consume(TokenKind::class_kw, "expected 'class'");
        Class declaration;
        declaration.name =
            consume(TokenKind::identifier, "expected class name");
        if (match(TokenKind::less)) {
            declaration.template_parameter =
                consume(TokenKind::identifier,
                        "expected generic type parameter")
                    .text;
            consume(TokenKind::greater,
                    "expected '>' after generic type parameter");
        }
        const auto previous_template_parameter = active_template_parameter_;
        active_template_parameter_ = declaration.template_parameter;
        consume(TokenKind::left_brace, "expected '{' after class name");
        while (current_.kind != TokenKind::right_brace &&
               current_.kind != TokenKind::end) {
            const bool is_template_type =
                !active_template_parameter_.empty() &&
                current_.kind == TokenKind::identifier &&
                current_.text == active_template_parameter_;
            if (current_.kind == TokenKind::type_kw || is_template_type) {
                const auto type = current_;
                advance();
                std::uint8_t pointer_depth = 0;
                while (match(TokenKind::star)) {
                    if (pointer_depth ==
                        std::numeric_limits<std::uint8_t>::max()) {
                        fail(type, "pointer type has too many indirections");
                    }
                    ++pointer_depth;
                }
                const auto name =
                    consume(TokenKind::identifier, "expected field name");
                consume(TokenKind::semicolon, "expected ';' after field");
                declaration.fields.push_back(
                    {name,
                     is_template_type ? ValueType::u64
                                      : value_type_from_name(type.text),
                     is_template_type, pointer_depth});
                continue;
            }
            declaration.methods.push_back(
                parse_function(declaration.name.text));
        }
        consume(TokenKind::right_brace, "expected '}' after class body");
        active_template_parameter_ = previous_template_parameter;
        return declaration;
    }

    std::vector<std::unique_ptr<Stmt>> parse_block_contents() {
        consume(TokenKind::left_brace, "expected '{'");
        std::vector<std::unique_ptr<Stmt>> statements;
        while (current_.kind != TokenKind::right_brace &&
               current_.kind != TokenKind::end) {
            statements.push_back(parse_statement());
        }
        consume(TokenKind::right_brace, "expected '}'");
        return statements;
    }

    std::unique_ptr<Stmt> parse_statement() {
        if (current_.kind == TokenKind::left_brace) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::block;
            statement->token = current_;
            statement->statements = parse_block_contents();
            return statement;
        }
        if (match(TokenKind::return_kw)) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::return_value;
            statement->token = current_;
            statement->expression = parse_expression();
            consume(TokenKind::semicolon, "expected ';' after return value");
            return statement;
        }
        if (current_.kind == TokenKind::type_kw ||
            (current_.kind == TokenKind::identifier &&
             (next_.kind == TokenKind::identifier ||
              next_.kind == TokenKind::star ||
              (next_.kind == TokenKind::less &&
               after_next_.kind == TokenKind::type_kw &&
               following_.kind == TokenKind::greater)))) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::variable;
            statement->token = current_;
            if (current_.kind == TokenKind::type_kw) {
                statement->value_type = value_type_from_name(current_.text);
                advance();
            } else if (!active_template_parameter_.empty() &&
                       current_.text == active_template_parameter_) {
                statement->template_type = true;
                advance();
            } else {
                statement->value_type = ValueType::u64;
                statement->class_name = current_.text;
                advance();
                if (match(TokenKind::less)) {
                    const auto argument = consume(
                        TokenKind::type_kw,
                        "expected a core generic type argument");
                    statement->class_name +=
                        "<" + std::string(value_type_name(
                                  value_type_from_name(argument.text))) +
                        ">";
                    consume(TokenKind::greater,
                            "expected '>' after generic type argument");
                }
            }
            while (match(TokenKind::star)) {
                if (statement->pointer_depth ==
                    std::numeric_limits<std::uint8_t>::max()) {
                    fail(statement->token,
                         "pointer type has too many indirections");
                }
                ++statement->pointer_depth;
            }
            const auto name =
                consume(TokenKind::identifier, "expected variable name");
            statement->name = name.text;
            if (match(TokenKind::left_bracket)) {
                const auto length = consume(
                    TokenKind::integer,
                    "expected a constant array length");
                const bool hexadecimal =
                    length.text.size() > 2 && length.text[0] == '0' &&
                    (length.text[1] == 'x' || length.text[1] == 'X');
                const auto* begin =
                    length.text.data() + (hexadecimal ? 2 : 0);
                const auto* end = length.text.data() + length.text.size();
                std::uint64_t parsed = 0;
                const auto result = std::from_chars(
                    begin, end, parsed, hexadecimal ? 16 : 10);
                if (result.ec != std::errc{} || result.ptr != end ||
                    parsed == 0 ||
                    parsed > std::numeric_limits<std::uint32_t>::max()) {
                    fail(length,
                         "array length must be between 1 and 4294967295");
                }
                statement->array_length =
                    static_cast<std::uint32_t>(parsed);
                consume(TokenKind::right_bracket,
                        "expected ']' after array length");
            }
            if (match(TokenKind::assign)) {
                if (statement->array_length != 0) {
                    fail(statement->token,
                         "array initializers are not supported yet");
                }
                statement->expression = parse_expression();
            }
            consume(TokenKind::semicolon,
                    "expected ';' after variable declaration");
            return statement;
        }
        if (match(TokenKind::if_kw)) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::if_statement;
            statement->token = current_;
            consume(TokenKind::left_paren, "expected '(' after 'if'");
            statement->expression = parse_expression();
            consume(TokenKind::right_paren, "expected ')' after condition");
            statement->first = parse_statement();
            if (match(TokenKind::else_kw)) {
                statement->second = parse_statement();
            }
            return statement;
        }
        if (match(TokenKind::while_kw)) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::while_statement;
            statement->token = current_;
            consume(TokenKind::left_paren, "expected '(' after 'while'");
            statement->expression = parse_expression();
            consume(TokenKind::right_paren, "expected ')' after condition");
            statement->first = parse_statement();
            return statement;
        }
        auto statement = std::make_unique<Stmt>();
        statement->kind = Stmt::Kind::expression;
        statement->token = current_;
        auto first_expression = parse_expression();
        if (match(TokenKind::assign)) {
            const bool indirect =
                (first_expression->kind == Expr::Kind::unary &&
                 first_expression->op == TokenKind::star) ||
                first_expression->kind == Expr::Kind::index;
            if (first_expression->kind != Expr::Kind::variable &&
                first_expression->kind != Expr::Kind::member && !indirect) {
                fail(first_expression->token, "invalid assignment target");
            }
            statement->kind =
                indirect ? Stmt::Kind::indirect_assign
                         : (first_expression->kind == Expr::Kind::member
                                ? Stmt::Kind::field_assign
                                : Stmt::Kind::assign);
            statement->name = first_expression->name;
            statement->target = std::move(first_expression);
            statement->expression = parse_expression();
            consume(TokenKind::semicolon, "expected ';' after assignment");
        } else {
            statement->expression = std::move(first_expression);
            consume(TokenKind::semicolon, "expected ';' after expression");
        }
        return statement;
    }

    std::unique_ptr<Expr> parse_expression() { return parse_equality(); }

    std::unique_ptr<Expr> parse_equality() {
        auto expression = parse_comparison();
        while (current_.kind == TokenKind::equal ||
               current_.kind == TokenKind::not_equal) {
            expression = make_binary(std::move(expression));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_comparison() {
        auto expression = parse_term();
        while (current_.kind == TokenKind::less ||
               current_.kind == TokenKind::less_equal ||
               current_.kind == TokenKind::greater ||
               current_.kind == TokenKind::greater_equal) {
            expression = make_binary(std::move(expression));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_term() {
        auto expression = parse_factor();
        while (current_.kind == TokenKind::plus ||
               current_.kind == TokenKind::minus) {
            expression = make_binary(std::move(expression));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_factor() {
        auto expression = parse_unary();
        while (current_.kind == TokenKind::star ||
               current_.kind == TokenKind::slash ||
               current_.kind == TokenKind::percent) {
            expression = make_binary(std::move(expression));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_unary() {
        if (current_.kind == TokenKind::minus ||
            current_.kind == TokenKind::bang ||
            current_.kind == TokenKind::ampersand ||
            current_.kind == TokenKind::star) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::unary;
            expression->token = current_;
            expression->op = current_.kind;
            advance();
            expression->right = parse_unary();
            return expression;
        }
        return parse_postfix();
    }

    std::unique_ptr<Expr> parse_postfix() {
        auto expression = parse_primary();
        for (;;) {
            if (current_.kind == TokenKind::left_bracket) {
                const auto bracket = current_;
                advance();
                auto access = std::make_unique<Expr>();
                access->kind = Expr::Kind::index;
                access->token = bracket;
                access->left = std::move(expression);
                access->right = parse_expression();
                consume(TokenKind::right_bracket,
                        "expected ']' after array index");
                expression = std::move(access);
                continue;
            }
            if (!match(TokenKind::dot)) {
                break;
            }
            const auto member =
                consume(TokenKind::identifier, "expected member name");
            auto access = std::make_unique<Expr>();
            access->token = member;
            access->name = member.text;
            access->left = std::move(expression);
            if (match(TokenKind::left_paren)) {
                access->kind = Expr::Kind::call;
                if (current_.kind != TokenKind::right_paren) {
                    do {
                        access->arguments.push_back(parse_expression());
                    } while (match(TokenKind::comma));
                }
                consume(TokenKind::right_paren,
                        "expected ')' after method arguments");
            } else {
                access->kind = Expr::Kind::member;
            }
            expression = std::move(access);
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_primary() {
        if (current_.kind == TokenKind::string_literal) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::literal;
            expression->token = current_;
            expression->value_type = ValueType::text;
            expression->text = decode_string(current_);
            advance();
            return expression;
        }
        if (current_.kind == TokenKind::integer) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::literal;
            expression->token = current_;
            const bool hexadecimal =
                current_.text.size() > 2 && current_.text[0] == '0' &&
                (current_.text[1] == 'x' || current_.text[1] == 'X');
            const auto* begin = current_.text.data() + (hexadecimal ? 2 : 0);
            const auto* end = begin + current_.text.size();
            if (hexadecimal) {
                end -= 2;
            }
            const auto result = std::from_chars(
                begin, end, expression->bits, hexadecimal ? 16 : 10);
            if (result.ec != std::errc{} || result.ptr != end) {
                fail(current_, "integer literal is outside the u64 range");
            }
            expression->value_type =
                expression->bits <=
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())
                    ? ValueType::i64
                    : ValueType::u64;
            advance();
            return expression;
        }
        if (current_.kind == TokenKind::floating) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::literal;
            expression->token = current_;
            double value = 0.0;
            const auto* begin = current_.text.data();
            const auto* end = begin + current_.text.size();
            const auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc{} || result.ptr != end) {
                fail(current_, "invalid f64 literal");
            }
            expression->value_type = ValueType::f64;
            expression->bits = std::bit_cast<std::uint64_t>(value);
            advance();
            return expression;
        }
        if (current_.kind == TokenKind::true_kw ||
            current_.kind == TokenKind::false_kw) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::literal;
            expression->token = current_;
            expression->value_type = ValueType::boolean;
            expression->bits = current_.kind == TokenKind::true_kw ? 1 : 0;
            advance();
            return expression;
        }
        if (current_.kind == TokenKind::type_kw) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::cast;
            expression->token = current_;
            expression->value_type = value_type_from_name(current_.text);
            advance();
            consume(TokenKind::left_paren, "expected '(' after cast type");
            expression->right = parse_expression();
            consume(TokenKind::right_paren, "expected ')' after cast value");
            return expression;
        }
        if (current_.kind == TokenKind::identifier &&
            current_.text == "ptr_cast" && next_.kind == TokenKind::less) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::pointer_cast;
            expression->token = current_;
            advance();
            consume(TokenKind::less, "expected '<' after ptr_cast");
            const auto target =
                consume(TokenKind::type_kw, "expected ptr_cast target type");
            expression->value_type = value_type_from_name(target.text);
            consume(TokenKind::greater, "expected '>' after ptr_cast type");
            consume(TokenKind::left_paren,
                    "expected '(' after ptr_cast type");
            expression->right = parse_expression();
            consume(TokenKind::right_paren,
                    "expected ')' after ptr_cast address");
            return expression;
        }
        if (current_.kind == TokenKind::identifier &&
            next_.kind == TokenKind::less &&
            after_next_.kind == TokenKind::type_kw &&
            following_.kind == TokenKind::greater) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::call;
            expression->token = current_;
            expression->name = current_.text;
            advance();
            consume(TokenKind::less, "expected '<' after generic class name");
            const auto argument = consume(
                TokenKind::type_kw,
                "expected a core generic type argument");
            expression->name +=
                "<" + std::string(value_type_name(
                          value_type_from_name(argument.text))) +
                ">";
            consume(TokenKind::greater,
                    "expected '>' after generic type argument");
            consume(TokenKind::left_paren,
                    "expected '(' after generic class type");
            if (current_.kind != TokenKind::right_paren) {
                do {
                    expression->arguments.push_back(parse_expression());
                } while (match(TokenKind::comma));
            }
            consume(TokenKind::right_paren,
                    "expected ')' after constructor arguments");
            return expression;
        }
        if (current_.kind == TokenKind::identifier) {
            auto expression = std::make_unique<Expr>();
            expression->token = current_;
            expression->name = current_.text;
            advance();
            if (match(TokenKind::left_paren)) {
                expression->kind = Expr::Kind::call;
                if (current_.kind != TokenKind::right_paren) {
                    do {
                        expression->arguments.push_back(parse_expression());
                    } while (match(TokenKind::comma));
                }
                consume(TokenKind::right_paren, "expected ')' after arguments");
            } else {
                expression->kind = Expr::Kind::variable;
            }
            return expression;
        }
        if (match(TokenKind::left_paren)) {
            auto expression = parse_expression();
            consume(TokenKind::right_paren, "expected ')' after expression");
            return expression;
        }
        fail(current_, "expected expression");
    }

    std::string decode_string(const Token& token) const {
        std::string value;
        value.reserve(token.text.size());
        for (std::size_t index = 0; index < token.text.size(); ++index) {
            const char character = token.text[index];
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (++index >= token.text.size()) {
                fail(token, "unterminated string escape");
            }
            switch (token.text[index]) {
            case '\\':
                value.push_back('\\');
                break;
            case '"':
                value.push_back('"');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case '0':
                value.push_back('\0');
                break;
            default:
                fail(token, "unsupported string escape");
            }
        }
        return value;
    }

    std::unique_ptr<Expr> make_binary(std::unique_ptr<Expr> left) {
        auto expression = std::make_unique<Expr>();
        expression->kind = Expr::Kind::binary;
        expression->token = current_;
        expression->op = current_.kind;
        expression->left = std::move(left);
        advance();
        if (expression->op == TokenKind::equal ||
            expression->op == TokenKind::not_equal) {
            expression->right = parse_comparison();
        } else if (expression->op == TokenKind::less ||
                   expression->op == TokenKind::less_equal ||
                   expression->op == TokenKind::greater ||
                   expression->op == TokenKind::greater_equal) {
            expression->right = parse_term();
        } else if (expression->op == TokenKind::plus ||
                   expression->op == TokenKind::minus) {
            expression->right = parse_factor();
        } else {
            expression->right = parse_unary();
        }
        return expression;
    }
};

struct LoadedProgram {
    std::vector<Class> classes;
    std::vector<Function> functions;
    bool has_std{};
};

class ModuleLoader {
public:
    ModuleLoader(const std::string_view filename,
                 const std::string_view module_root)
        : root_display_(filename), module_root_(module_root) {
        if (!filename.empty() && filename.front() != '<') {
            root_path_ = std::filesystem::absolute(
                             std::filesystem::path(filename))
                             .lexically_normal();
            root_directory_ = root_path_.parent_path();
        } else {
            root_directory_ = std::filesystem::current_path();
        }
    }

    LoadedProgram load(const std::string_view source) {
        const auto root_key = root_path_.empty()
                                  ? std::string("<root>")
                                  : path_key(root_path_);
        load_source(source, root_path_, root_display_, root_key);
        if (functions_.empty()) {
            throw CompileError(root_display_ + ": expected at least one function");
        }
        return {std::move(classes_), std::move(functions_), has_std_};
    }

private:
    std::string root_display_;
    std::filesystem::path root_path_;
    std::filesystem::path root_directory_;
    std::filesystem::path module_root_;
    std::vector<Class> classes_;
    std::vector<Function> functions_;
    std::unordered_set<std::string> loaded_;
    std::unordered_set<std::string> active_;
    bool has_std_{};

    static std::string path_key(const std::filesystem::path& path) {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(path, error);
        if (error) {
            normalized = std::filesystem::absolute(path).lexically_normal();
        }
#ifdef _WIN32
        auto key = normalized.generic_string();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return key;
#else
        return normalized.generic_string();
#endif
    }

    std::filesystem::path resolve(const Import& import,
                                  const std::filesystem::path& importer) const {
        auto module_name = std::string(import.name.text);
        std::size_t separator = 0;
        while ((separator = module_name.find("::", separator)) !=
               std::string::npos) {
            module_name.replace(separator, 2, "/");
            ++separator;
        }
        const auto filename = std::filesystem::path(module_name + ".concept");
        std::vector<std::filesystem::path> candidates;
        if (!importer.empty()) {
            const auto directory = importer.parent_path();
            candidates.push_back(directory / "concept" / filename);
            if (directory.filename() == "concept") {
                candidates.push_back(directory / filename);
            }
        }
        candidates.push_back(root_directory_ / "concept" / filename);
        if (!module_root_.empty()) {
            candidates.push_back(module_root_ / filename);
        }
        candidates.push_back(std::filesystem::current_path() / "concept" /
                             filename);

        std::unordered_set<std::string> visited;
        for (const auto& candidate : candidates) {
            const auto key = path_key(candidate);
            if (!visited.insert(key).second) {
                continue;
            }
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return std::filesystem::absolute(candidate).lexically_normal();
            }
        }

        throw CompileError(import.name.filename + ':' +
                           std::to_string(import.name.line) + ':' +
                           std::to_string(import.name.column) +
                           ": cannot find module '" +
                           std::string(import.name.text) + "'");
    }

    void load_import(const Import& import,
                     const std::filesystem::path& importer) {
        const auto path = resolve(import, importer);
        const auto key = path_key(path);
        if (active_.contains(key)) {
            throw CompileError(import.name.filename + ':' +
                               std::to_string(import.name.line) + ':' +
                               std::to_string(import.name.column) +
                               ": import cycle through module '" +
                               std::string(import.name.text) + "'");
        }
        if (loaded_.contains(key)) {
            return;
        }

        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            throw CompileError("cannot open imported module: " + path.string());
        }
        const std::string imported_source{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
        load_source(imported_source, path, path.string(), key);
    }

    void load_source(const std::string_view source,
                     const std::filesystem::path& path,
                     const std::string& display_name,
                     const std::string& key) {
        active_.insert(key);
        Parser parser(source, display_name);
        auto module = parser.parse_program();
        for (const auto& import : module.imports) {
            if (import.name.text == "std::socket") {
                has_std_ = true;
            }
            load_import(import, path);
        }
        for (auto& declaration : module.classes) {
            classes_.push_back(std::move(declaration));
        }
        for (auto& function : module.functions) {
            functions_.push_back(std::move(function));
        }
        active_.erase(key);
        loaded_.insert(key);
    }
};

struct FunctionInfo {
    std::uint32_t entry{};
    std::uint32_t locals{};
    ValueType return_type{ValueType::i64};
};

struct LocalInfo {
    std::uint16_t index{};
    ValueType type{ValueType::i64};
    std::string class_name;
    std::uint8_t pointer_depth{};
    std::uint32_t array_length{};
};

struct FieldInfo {
    std::uint16_t index{};
    ValueType type{ValueType::i64};
    std::uint8_t pointer_depth{};
};

struct SemanticType {
    ValueType type{ValueType::i64};
    std::string class_name;
    std::uint8_t pointer_depth{};
};

struct ClassInfo {
    std::uint16_t field_count{};
    std::unordered_map<std::string, FieldInfo> fields;
    std::unordered_map<std::string, std::string> methods;
    bool native_socket{};
};

struct GenericSpecialization {
    const Class* declaration{};
    std::string name;
    ValueType argument{ValueType::i64};
};

struct CallPatch {
    std::size_t target_offset{};
    std::size_t locals_offset{};
    Token token;
    std::string name;
};

class CodeGenerator {
public:
    CodeGenerator(std::vector<Class>& classes,
                  std::vector<Function>& functions,
                  const std::string_view filename, const bool has_std)
        : classes_(classes), functions_(functions), filename_(filename),
          has_std_(has_std) {}

    Bytecode generate() {
        register_classes();
        register_functions();
        for (const auto& function : functions_) {
            compile_function(function);
        }
        for (const auto& declaration : classes_) {
            if (!declaration.template_parameter.empty()) {
                continue;
            }
            for (const auto& method : declaration.methods) {
                compile_function(method);
            }
        }
        for (const auto& specialization : generic_specializations_) {
            for (const auto& method : specialization.declaration->methods) {
                compile_function(method, specialization.name,
                                 specialization.argument);
            }
        }
        resolve_calls();

        const auto main = function_info_.find("main");
        if (main == function_info_.end()) {
            throw CompileError(std::string(filename_) +
                               ": program has no 'main' function");
        }

        Bytecode result;
        result.code = std::move(code_);
        result.entry = main->second.entry;
        result.entry_locals = main->second.locals;
        result.entry_type = main->second.return_type;
        result.strings = std::move(strings_);
        if (!is_integral(result.entry_type) &&
            result.entry_type != ValueType::boolean) {
            throw CompileError(std::string(filename_) +
                               ": 'main' must return an integral type or bool");
        }
        validate(result);
        return result;
    }

private:
    std::vector<Class>& classes_;
    std::vector<Function>& functions_;
    std::string_view filename_;
    std::vector<std::uint8_t> code_;
    std::unordered_map<std::string, FunctionInfo> function_info_;
    std::unordered_map<std::string, ClassInfo> class_info_;
    std::unordered_map<std::string, const Class*> class_templates_;
    std::vector<GenericSpecialization> generic_specializations_;
    std::unordered_set<std::string> generic_specialization_names_;
    std::unordered_map<std::string, LocalInfo> locals_;
    std::vector<CallPatch> call_patches_;
    std::vector<std::string> strings_;
    ValueType current_return_type_{ValueType::i64};
    std::string current_class_;
    std::optional<ValueType> current_template_type_;
    std::uint8_t current_complexity_{};
    std::uint32_t obfuscation_credit_{};
    std::uint64_t obfuscation_state_{fresh_opcode_seed()};
    bool has_std_{};

    [[noreturn]] void fail(const Token& token,
                           const std::string& message) const {
        throw CompileError(token.filename + ':' +
                           std::to_string(token.line) + ':' +
                           std::to_string(token.column) + ": " + message);
    }

    static std::string function_key(const Function& function) {
        return function.class_name.empty()
                   ? function.name.text
                   : function.class_name + "." + function.name.text;
    }

    static std::optional<std::pair<std::string, ValueType>>
    generic_application(const std::string& name) {
        const auto open = name.find('<');
        if (open == std::string::npos || name.empty() || name.back() != '>' ||
            open == 0 || open + 2 >= name.size()) {
            return std::nullopt;
        }
        const auto argument = std::string_view(name).substr(
            open + 1, name.size() - open - 2);
        if (!is_type_name(argument)) {
            return std::nullopt;
        }
        return std::pair{name.substr(0, open),
                         value_type_from_name(argument)};
    }

    void register_concrete_class(const Class& declaration,
                                 const std::string& name,
                                 const std::optional<ValueType> argument) {
        if (declaration.fields.size() >
            std::numeric_limits<std::uint16_t>::max()) {
            fail(declaration.name, "class has too many fields");
        }
        ClassInfo info;
        info.field_count =
            static_cast<std::uint16_t>(declaration.fields.size());
        for (std::size_t index = 0; index < declaration.fields.size();
             ++index) {
            const auto& field = declaration.fields[index];
            if (field.template_type && !argument) {
                fail(field.name,
                     "generic field type requires a specialization");
            }
            const auto type = field.template_type ? *argument : field.type;
            if (!info.fields
                     .emplace(field.name.text,
                              FieldInfo{static_cast<std::uint16_t>(index),
                                        type, field.pointer_depth})
                     .second) {
                fail(field.name,
                     "duplicate field '" + field.name.text + "'");
            }
        }
        for (const auto& method : declaration.methods) {
            if (!info.methods
                     .emplace(method.name.text,
                              name + "." + method.name.text)
                     .second) {
                fail(method.name,
                     "duplicate method '" + method.name.text + "'");
            }
        }
        if (!class_info_.emplace(name, std::move(info)).second) {
            fail(declaration.name, "duplicate class '" + name + "'");
        }
    }

    void collect_generic_expression(const Expr& expression) {
        if (expression.kind == Expr::Kind::call && !expression.left) {
            register_generic_use(expression.name, expression.token);
        }
        if (expression.left) {
            collect_generic_expression(*expression.left);
        }
        if (expression.right) {
            collect_generic_expression(*expression.right);
        }
        for (const auto& argument : expression.arguments) {
            collect_generic_expression(*argument);
        }
    }

    void collect_generic_statement(const Stmt& statement) {
        register_generic_use(statement.class_name, statement.token);
        if (statement.target) {
            collect_generic_expression(*statement.target);
        }
        if (statement.expression) {
            collect_generic_expression(*statement.expression);
        }
        if (statement.first) {
            collect_generic_statement(*statement.first);
        }
        if (statement.second) {
            collect_generic_statement(*statement.second);
        }
        for (const auto& child : statement.statements) {
            collect_generic_statement(*child);
        }
    }

    void register_generic_use(const std::string& name, const Token& token) {
        const auto application = generic_application(name);
        if (!application) {
            return;
        }
        if (!generic_specialization_names_.insert(name).second) {
            return;
        }
        const auto declaration = class_templates_.find(application->first);
        if (declaration == class_templates_.end()) {
            fail(token, "unknown generic class '" + application->first +
                            "'");
        }
        generic_specializations_.push_back(
            {declaration->second, name, application->second});
    }

    void register_classes() {
        if (has_std_) {
            ClassInfo socket;
            socket.native_socket = true;
            class_info_.emplace("std::Socket", std::move(socket));
        }
        for (const auto& declaration : classes_) {
            const auto name = declaration.name.text;
            if (is_builtin_name(name)) {
                fail(declaration.name,
                     "class name '" + name + "' is reserved by the VM");
            }
            if (!declaration.template_parameter.empty()) {
                if (class_info_.contains(name) ||
                    !class_templates_.emplace(name, &declaration).second) {
                    fail(declaration.name,
                         "duplicate class '" + name + "'");
                }
            } else {
                if (class_templates_.contains(name)) {
                    fail(declaration.name,
                         "duplicate class '" + name + "'");
                }
                register_concrete_class(declaration, name, std::nullopt);
            }
        }

        const auto collect_function = [&](const Function& function) {
            for (const auto& statement : function.body) {
                collect_generic_statement(*statement);
            }
        };
        for (const auto& function : functions_) {
            collect_function(function);
        }
        for (const auto& declaration : classes_) {
            for (const auto& method : declaration.methods) {
                collect_function(method);
            }
        }
        for (const auto& specialization : generic_specializations_) {
            register_concrete_class(*specialization.declaration,
                                    specialization.name,
                                    specialization.argument);
        }
    }

    void register_functions() {
        for (const auto& function : functions_) {
            const std::string name(function.name.text);
            if (is_builtin_name(name)) {
                fail(function.name,
                     "function name '" + name + "' is reserved by the VM");
            }
            if (class_info_.contains(name)) {
                fail(function.name,
                     "function conflicts with class '" + name + "'");
            }
            if (!function_info_
                     .emplace(name,
                              FunctionInfo{0, 0, function.return_type})
                     .second) {
                fail(function.name, "duplicate function '" + name + "'");
            }
        }
        for (const auto& declaration : classes_) {
            if (!declaration.template_parameter.empty()) {
                continue;
            }
            for (const auto& method : declaration.methods) {
                const auto key = function_key(method);
                function_info_.emplace(
                    key, FunctionInfo{0, 0, method.return_type});
            }
        }
        for (const auto& specialization : generic_specializations_) {
            for (const auto& method : specialization.declaration->methods) {
                const auto key =
                    specialization.name + "." + method.name.text;
                const auto return_type = method.return_template_type
                                             ? specialization.argument
                                             : method.return_type;
                function_info_.emplace(
                    key, FunctionInfo{0, 0, return_type});
            }
        }
    }

    void compile_function(
        const Function& function,
        const std::string_view class_name_override = {},
        const std::optional<ValueType> template_type = std::nullopt) {
        const auto class_name = class_name_override.empty()
                                    ? function.class_name
                                    : std::string(class_name_override);
        const auto key = class_name.empty()
                             ? function.name.text
                             : class_name + "." + function.name.text;
        auto& info = function_info_.at(key);
        info.entry = checked_offset();
        if (function.return_template_type && !template_type) {
            fail(function.name,
                 "generic return type requires a specialization");
        }
        current_template_type_ = template_type;
        current_return_type_ = function.return_template_type
                                   ? *template_type
                                   : function.return_type;
        current_class_ = class_name;
        current_complexity_ = function.complexity;
        obfuscation_credit_ = 0;
        locals_.clear();
        if (!current_class_.empty()) {
            locals_.emplace(
                "this", LocalInfo{0, ValueType::u64, current_class_, 0});
        }
        if (current_complexity_ != 0) {
            emit_obfuscation_layer();
        }
        for (const auto& statement : function.body) {
            compile_statement(*statement);
        }

        emit_obfuscation();
        emit_default_value(current_return_type_);
        emit(Op::return_value);
        info.locals = static_cast<std::uint32_t>(locals_.size());
    }

    void compile_statement(const Stmt& statement) {
        emit_obfuscation();
        switch (statement.kind) {
        case Stmt::Kind::block:
            for (const auto& child : statement.statements) {
                compile_statement(*child);
            }
            return;
        case Stmt::Kind::variable: {
            if (locals_.contains(statement.name)) {
                fail(statement.token,
                     "duplicate variable '" + statement.name + "'");
            }
            if (locals_.size() >= std::numeric_limits<std::uint16_t>::max()) {
                fail(statement.token, "too many local variables");
            }
            if (statement.template_type && !current_template_type_) {
                fail(statement.token,
                     "generic local type requires a specialization");
            }
            const auto value_type = statement.template_type
                                        ? *current_template_type_
                                        : statement.value_type;
            const auto index = static_cast<std::uint16_t>(locals_.size());
            locals_.emplace(statement.name,
                            LocalInfo{index, value_type,
                                      statement.class_name,
                                      statement.pointer_depth,
                                      statement.array_length});
            if (statement.array_length != 0) {
                if (!statement.class_name.empty() &&
                    statement.pointer_depth == 0) {
                    fail(statement.token,
                         "arrays of class objects are not supported yet");
                }
                if (!statement.class_name.empty()) {
                    lookup_class(statement.token, statement.class_name);
                }
                if (statement.pointer_depth ==
                    std::numeric_limits<std::uint8_t>::max()) {
                    fail(statement.token,
                         "array element pointer type has too many "
                         "indirections");
                }
                emit(Op::array_alloc);
                emit_type(statement.pointer_depth == 0
                              ? value_type
                              : ValueType::u64);
                emit_u32(statement.array_length);
            } else if (statement.pointer_depth != 0) {
                if (!statement.class_name.empty()) {
                    lookup_class(statement.token, statement.class_name);
                }
                if (statement.expression) {
                    compile_pointer_expression_as(
                        *statement.expression,
                        SemanticType{value_type,
                                     statement.class_name,
                                     statement.pointer_depth});
                } else {
                    emit(Op::push_bits);
                    emit_u64(0);
                }
            } else if (!statement.class_name.empty()) {
                lookup_class(statement.token, statement.class_name);
                if (statement.expression) {
                    compile_object_expression_as(*statement.expression,
                                                 statement.class_name);
                } else {
                    emit_new_object(statement.class_name);
                }
            } else if (statement.expression) {
                compile_expression_as(*statement.expression,
                                      value_type);
            } else {
                emit_default_value(value_type);
            }
            emit(Op::store);
            emit_u16(index);
            return;
        }
        case Stmt::Kind::assign: {
            const auto& local = lookup_local(statement.token, statement.name);
            if (local.array_length != 0) {
                fail(statement.token, "cannot assign to an array");
            }
            if (local.pointer_depth != 0) {
                compile_pointer_expression_as(
                    *statement.expression,
                    SemanticType{local.type, local.class_name,
                                 local.pointer_depth});
            } else if (local.class_name.empty()) {
                compile_expression_as(*statement.expression, local.type);
            } else {
                compile_object_expression_as(*statement.expression,
                                             local.class_name);
            }
            emit(Op::store);
            emit_u16(local.index);
            return;
        }
        case Stmt::Kind::field_assign: {
            const auto class_name =
                expression_class(*statement.target->left);
            if (class_name.empty()) {
                fail(statement.target->token,
                     "field assignment requires a class object");
            }
            const auto& field = lookup_field(statement.target->token,
                                             class_name, statement.name);
            compile_object_expression_as(*statement.target->left, class_name);
            if (field.pointer_depth != 0) {
                compile_pointer_expression_as(
                    *statement.expression,
                    SemanticType{field.type, {}, field.pointer_depth});
            } else {
                compile_expression_as(*statement.expression, field.type);
            }
            emit(Op::store_field);
            emit_u16(field.index);
            return;
        }
        case Stmt::Kind::indirect_assign: {
            SemanticType target_type;
            if (statement.target->kind == Expr::Kind::index) {
                target_type = semantic_type(*statement.target);
                compile_index_address(*statement.target);
            } else {
                const auto pointer_type =
                    semantic_type(*statement.target->right);
                if (pointer_type.pointer_depth == 0) {
                    fail(statement.target->token,
                         "indirect assignment requires a pointer");
                }
                static_cast<void>(
                    compile_expression(*statement.target->right));
                target_type = pointer_type;
                --target_type.pointer_depth;
            }
            compile_semantic_expression_as(*statement.expression,
                                           target_type);
            emit(Op::store_indirect);
            return;
        }
        case Stmt::Kind::expression:
            static_cast<void>(compile_expression(*statement.expression));
            emit(Op::pop);
            return;
        case Stmt::Kind::return_value:
            compile_expression_as(*statement.expression, current_return_type_);
            emit(Op::return_value);
            return;
        case Stmt::Kind::if_statement: {
            compile_expression_as(*statement.expression, ValueType::boolean);
            emit(Op::jump_if_false);
            const auto false_target = reserve_u32();
            compile_statement(*statement.first);
            if (statement.second) {
                emit(Op::jump);
                const auto end_target = reserve_u32();
                patch_u32(false_target, checked_offset());
                compile_statement(*statement.second);
                patch_u32(end_target, checked_offset());
            } else {
                patch_u32(false_target, checked_offset());
            }
            return;
        }
        case Stmt::Kind::while_statement: {
            const auto loop_start = checked_offset();
            compile_expression_as(*statement.expression, ValueType::boolean);
            emit(Op::jump_if_false);
            const auto loop_end = reserve_u32();
            compile_statement(*statement.first);
            emit(Op::jump);
            emit_u32(loop_start);
            patch_u32(loop_end, checked_offset());
            return;
        }
        }
    }

    ValueType compile_expression(const Expr& expression) {
        switch (expression.kind) {
        case Expr::Kind::literal:
            if (expression.value_type == ValueType::text) {
                emit(Op::push_text);
                emit_u32(add_string(expression.text));
            } else {
                emit(Op::push_bits);
                emit_u64(expression.bits);
            }
            return expression.value_type;
        case Expr::Kind::variable: {
            const auto& local = lookup_local(expression.token, expression.name);
            emit(Op::load);
            emit_u16(local.index);
            return local.pointer_depth == 0 && local.array_length == 0
                       ? local.type
                       : ValueType::u64;
        }
        case Expr::Kind::index: {
            const auto result = semantic_type(expression);
            compile_index_address(expression);
            emit(Op::load_indirect);
            return result.pointer_depth != 0 || !result.class_name.empty()
                       ? ValueType::u64
                       : result.type;
        }
        case Expr::Kind::member: {
            const auto class_name = expression_class(*expression.left);
            if (class_name.empty()) {
                fail(expression.token,
                     "field access requires a class object");
            }
            const auto& field =
                lookup_field(expression.token, class_name, expression.name);
            compile_object_expression_as(*expression.left, class_name);
            emit(Op::load_field);
            emit_u16(field.index);
            return field.pointer_depth == 0 ? field.type : ValueType::u64;
        }
        case Expr::Kind::call: {
            if (expression.left) {
                const auto class_name = expression_class(*expression.left);
                if (class_name.empty()) {
                    fail(expression.token,
                         "method call requires a class object");
                }
                if (is_native_socket_class(class_name)) {
                    return compile_socket_method(expression);
                }
                if (!expression.arguments.empty()) {
                    fail(expression.token,
                         "method parameters are not supported yet");
                }
                const auto& method = lookup_method(
                    expression.token, class_name, expression.name);
                const auto& function =
                    lookup_function(expression.token, method);
                compile_object_expression_as(*expression.left, class_name);
                emit(Op::call_method);
                const auto target_offset = reserve_u32();
                const auto locals_offset = reserve_u32();
                call_patches_.push_back({target_offset, locals_offset,
                                         expression.token, method});
                return function.return_type;
            }
            if (class_info_.contains(expression.name)) {
                if (!expression.arguments.empty()) {
                    fail(expression.token,
                         "constructors do not accept arguments yet");
                }
                emit_new_object(expression.name);
                return ValueType::u64;
            }
            if (is_builtin_name(expression.name)) {
                return compile_builtin(expression);
            }
            const auto& function = lookup_function(expression.token,
                                                   expression.name);
            if (!expression.arguments.empty()) {
                fail(expression.token,
                     "user-defined function parameters are not supported yet");
            }
            emit(Op::call);
            const auto target_offset = reserve_u32();
            const auto locals_offset = reserve_u32();
            call_patches_.push_back(
                {target_offset, locals_offset, expression.token, expression.name});
            return function.return_type;
        }
        case Expr::Kind::cast: {
            static_cast<void>(semantic_type(expression));
            const auto source_type = compile_expression(*expression.right);
            emit_conversion(source_type, expression.value_type,
                            expression.token);
            return expression.value_type;
        }
        case Expr::Kind::pointer_cast:
            static_cast<void>(semantic_type(expression));
            compile_expression_as(*expression.right, ValueType::u64);
            emit(Op::native_pointer);
            emit_type(expression.value_type);
            return ValueType::u64;
        case Expr::Kind::unary: {
            if (expression.op == TokenKind::ampersand) {
                static_cast<void>(semantic_type(expression));
                compile_address(*expression.right);
                return ValueType::u64;
            }
            if (expression.op == TokenKind::star) {
                auto pointer_type = semantic_type(*expression.right);
                if (pointer_type.pointer_depth == 0) {
                    fail(expression.token,
                         "dereference operator requires a pointer");
                }
                static_cast<void>(compile_expression(*expression.right));
                emit(Op::load_indirect);
                --pointer_type.pointer_depth;
                return pointer_type.pointer_depth != 0 ||
                               !pointer_type.class_name.empty()
                           ? ValueType::u64
                           : pointer_type.type;
            }
            const auto operand = semantic_type(*expression.right);
            if (operand.pointer_depth != 0) {
                fail(expression.token,
                     "unary operator cannot use a pointer");
            }
            if (!operand.class_name.empty()) {
                fail(expression.token,
                     "unary operator cannot use class object '" +
                         operand.class_name + "'");
            }
            const auto operand_type = operand.type;
            static_cast<void>(compile_expression(*expression.right));
            if (expression.op == TokenKind::bang) {
                emit(Op::logical_not);
                emit_type(operand_type);
                return ValueType::boolean;
            }
            if (!is_numeric(operand_type)) {
                fail(expression.token, "unary '-' requires a numeric operand");
            }
            emit(Op::negate);
            emit_type(operand_type);
            return operand_type;
        }
        case Expr::Kind::binary: {
            const auto left_semantic = semantic_type(*expression.left);
            const auto right_semantic = semantic_type(*expression.right);
            if (left_semantic.pointer_depth != 0 ||
                right_semantic.pointer_depth != 0) {
                static_cast<void>(semantic_type(expression));
                const bool pointer_on_left =
                    left_semantic.pointer_depth != 0;
                const auto& pointer_expression =
                    pointer_on_left ? *expression.left : *expression.right;
                const auto& offset_expression =
                    pointer_on_left ? *expression.right : *expression.left;
                auto pointer_type = pointer_on_left ? left_semantic
                                                    : right_semantic;
                static_cast<void>(compile_expression(pointer_expression));
                compile_expression_as(offset_expression, ValueType::i64);
                if (expression.op == TokenKind::minus) {
                    emit(Op::negate);
                    emit_type(ValueType::i64);
                }
                --pointer_type.pointer_depth;
                emit(Op::pointer_offset);
                emit_type(pointer_type.pointer_depth != 0 ||
                                  !pointer_type.class_name.empty()
                              ? ValueType::u64
                              : pointer_type.type);
                return ValueType::u64;
            }
            const auto result_type = expression_type(expression);
            const auto left_type = expression_type(*expression.left);
            const auto right_type = expression_type(*expression.right);
            const auto operation_type =
                is_comparison(expression.op)
                    ? comparison_operand_type(expression, left_type, right_type)
                    : common_numeric_type(expression, left_type, right_type);
            compile_expression_as(*expression.left, operation_type);
            compile_expression_as(*expression.right, operation_type);
            emit(binary_op(expression));
            emit_type(operation_type);
            return result_type;
        }
        }
        throw std::logic_error("invalid expression kind");
    }

    void compile_expression_as(const Expr& expression,
                               const ValueType target_type) {
        const auto source = semantic_type(expression);
        if (source.pointer_depth != 0) {
            fail(expression.token,
                 "pointer cannot be used as a core value");
        }
        if (!source.class_name.empty()) {
            fail(expression.token, "class object '" + source.class_name +
                                       "' cannot be used as a core value");
        }
        const auto source_type = compile_expression(expression);
        emit_conversion(source_type, target_type, expression.token);
    }

    [[nodiscard]] SemanticType semantic_type(const Expr& expression) const {
        switch (expression.kind) {
        case Expr::Kind::literal:
            return {expression.value_type, {}, 0};
        case Expr::Kind::variable: {
            const auto& local =
                lookup_local(expression.token, expression.name);
            return {local.type, local.class_name,
                    static_cast<std::uint8_t>(
                        local.pointer_depth +
                        (local.array_length == 0 ? 0 : 1))};
        }
        case Expr::Kind::index: {
            auto pointer = semantic_type(*expression.left);
            if (pointer.pointer_depth == 0) {
                fail(expression.token,
                     "index operator requires an array or pointer");
            }
            const auto index = semantic_type(*expression.right);
            if (index.pointer_depth != 0 || !index.class_name.empty() ||
                !is_integral(index.type)) {
                fail(expression.right->token,
                     "array index must be an integral value");
            }
            --pointer.pointer_depth;
            return pointer;
        }
        case Expr::Kind::member: {
            const auto class_name = expression_class(*expression.left);
            if (class_name.empty()) {
                fail(expression.token,
                     "field access requires a class object");
            }
            const auto& field =
                lookup_field(expression.token, class_name, expression.name);
            return {field.type, {}, field.pointer_depth};
        }
        case Expr::Kind::call:
            if (expression.left) {
                const auto class_name = expression_class(*expression.left);
                if (class_name.empty()) {
                    fail(expression.token,
                         "method call requires a class object");
                }
                if (is_native_socket_class(class_name)) {
                    const auto type = socket_method_type(expression);
                    return expression.name == "accept"
                               ? SemanticType{type, "std::Socket", 0}
                               : SemanticType{type, {}, 0};
                }
                if (!expression.arguments.empty()) {
                    fail(expression.token,
                         "method parameters are not supported yet");
                }
                const auto& method = lookup_method(
                    expression.token, class_name, expression.name);
                return {lookup_function(expression.token, method).return_type,
                        {}, 0};
            }
            if (class_info_.contains(expression.name)) {
                if (!expression.arguments.empty()) {
                    fail(expression.token,
                         "constructors do not accept arguments yet");
                }
                return {ValueType::u64, expression.name, 0};
            }
            if (is_builtin_name(expression.name)) {
                if (expression.name == "malloc") {
                    static_cast<void>(builtin_type(expression));
                    return {ValueType::u8, {}, 1};
                }
                return {builtin_type(expression), {}, 0};
            }
            if (!expression.arguments.empty()) {
                fail(expression.token,
                     "user-defined function parameters are not supported yet");
            }
            return {lookup_function(expression.token, expression.name)
                        .return_type,
                    {}, 0};
        case Expr::Kind::cast: {
            const auto source = semantic_type(*expression.right);
            if (source.pointer_depth != 0) {
                fail(expression.token, "cannot cast a pointer");
            }
            if (!source.class_name.empty()) {
                fail(expression.token, "cannot cast class object '" +
                                           source.class_name + "'");
            }
            return {expression.value_type, {}, 0};
        }
        case Expr::Kind::pointer_cast: {
            const auto source = semantic_type(*expression.right);
            if (source.pointer_depth != 0 || !source.class_name.empty() ||
                !is_integral(source.type)) {
                fail(expression.token,
                     "ptr_cast address must be an integral value");
            }
            if (expression.value_type == ValueType::text) {
                fail(expression.token,
                     "ptr_cast<string> is not supported");
            }
            return {expression.value_type, {}, 1};
        }
        case Expr::Kind::unary: {
            auto operand = semantic_type(*expression.right);
            if (expression.op == TokenKind::ampersand) {
                const bool addressable =
                    expression.right->kind == Expr::Kind::variable ||
                    expression.right->kind == Expr::Kind::member ||
                    expression.right->kind == Expr::Kind::index ||
                    (expression.right->kind == Expr::Kind::unary &&
                     expression.right->op == TokenKind::star);
                if (!addressable) {
                    fail(expression.token,
                         "address-of operator requires a variable, field, or "
                         "dereference");
                }
                if (operand.pointer_depth ==
                    std::numeric_limits<std::uint8_t>::max()) {
                    fail(expression.token,
                         "pointer type has too many indirections");
                }
                ++operand.pointer_depth;
                return operand;
            }
            if (expression.op == TokenKind::star) {
                if (operand.pointer_depth == 0) {
                    fail(expression.token,
                         "dereference operator requires a pointer");
                }
                --operand.pointer_depth;
                return operand;
            }
            if (operand.pointer_depth != 0) {
                fail(expression.token,
                     "unary operator cannot use a pointer");
            }
            if (!operand.class_name.empty()) {
                fail(expression.token,
                     "unary operator cannot use a class object");
            }
            if (expression.op == TokenKind::bang) {
                return {ValueType::boolean, {}, 0};
            }
            if (!is_numeric(operand.type)) {
                fail(expression.token,
                     "unary '-' requires a numeric operand");
            }
            return operand;
        }
        case Expr::Kind::binary: {
            const auto left = semantic_type(*expression.left);
            const auto right = semantic_type(*expression.right);
            if (left.pointer_depth != 0 || right.pointer_depth != 0) {
                if (left.pointer_depth != 0 && right.pointer_depth != 0) {
                    fail(expression.token,
                         "two pointers cannot be used with a binary operator");
                }
                const bool pointer_on_left = left.pointer_depth != 0;
                const auto& pointer = pointer_on_left ? left : right;
                const auto& offset = pointer_on_left ? right : left;
                if (expression.op != TokenKind::plus &&
                    !(pointer_on_left &&
                      expression.op == TokenKind::minus)) {
                    fail(expression.token,
                         "pointer arithmetic supports only pointer + integer, "
                         "integer + pointer, and pointer - integer");
                }
                if (!offset.class_name.empty() ||
                    offset.pointer_depth != 0 ||
                    !is_integral(offset.type)) {
                    fail(expression.token,
                         "pointer offset must be an integral value");
                }
                return pointer;
            }
            if (!left.class_name.empty() || !right.class_name.empty()) {
                fail(expression.token,
                     "class objects cannot be used with binary operators");
            }
            if (is_comparison(expression.op)) {
                static_cast<void>(comparison_operand_type(
                    expression, left.type, right.type));
                return {ValueType::boolean, {}, 0};
            }
            const auto type =
                common_numeric_type(expression, left.type, right.type);
            if (expression.op == TokenKind::percent && is_floating(type)) {
                fail(expression.token,
                     "operator '%' requires integral operands");
            }
            return {type, {}, 0};
        }
        }
        throw std::logic_error("invalid expression kind");
    }

    void compile_pointer_expression_as(const Expr& expression,
                                       const SemanticType& expected) {
        if (expression.kind == Expr::Kind::call && !expression.left &&
            expression.name == "malloc") {
            compile_malloc(expression, expected);
            return;
        }
        const auto actual = semantic_type(expression);
        if (actual.pointer_depth == 0) {
            fail(expression.token, "expected a pointer value");
        }
        if (actual.pointer_depth != expected.pointer_depth ||
            actual.type != expected.type ||
            actual.class_name != expected.class_name) {
            fail(expression.token, "incompatible pointer type");
        }
        static_cast<void>(compile_expression(expression));
    }

    void compile_semantic_expression_as(const Expr& expression,
                                        const SemanticType& expected) {
        if (expected.pointer_depth != 0) {
            compile_pointer_expression_as(expression, expected);
        } else if (!expected.class_name.empty()) {
            compile_object_expression_as(expression, expected.class_name);
        } else {
            compile_expression_as(expression, expected.type);
        }
    }

    void compile_address(const Expr& expression) {
        if (expression.kind == Expr::Kind::index) {
            compile_index_address(expression);
            return;
        }
        if (expression.kind == Expr::Kind::variable) {
            const auto& local =
                lookup_local(expression.token, expression.name);
            emit(Op::address_local);
            emit_u16(local.index);
            return;
        }
        if (expression.kind == Expr::Kind::member) {
            const auto class_name = expression_class(*expression.left);
            if (class_name.empty()) {
                fail(expression.token,
                     "field address requires a class object");
            }
            const auto& field =
                lookup_field(expression.token, class_name, expression.name);
            compile_object_expression_as(*expression.left, class_name);
            emit(Op::address_field);
            emit_u16(field.index);
            return;
        }
        if (expression.kind == Expr::Kind::unary &&
            expression.op == TokenKind::star) {
            static_cast<void>(compile_expression(*expression.right));
            return;
        }
        fail(expression.token,
             "address-of operator requires a variable, field, or dereference");
    }

    void compile_index_address(const Expr& expression) {
        auto pointer = semantic_type(*expression.left);
        static_cast<void>(semantic_type(expression));
        static_cast<void>(compile_expression(*expression.left));
        compile_expression_as(*expression.right, ValueType::i64);
        --pointer.pointer_depth;
        emit(Op::pointer_offset);
        emit_type(pointer.pointer_depth != 0 || !pointer.class_name.empty()
                      ? ValueType::u64
                      : pointer.type);
    }

    [[nodiscard]] bool is_builtin_name(const std::string_view name) const {
        return name == "input" || name == "input_text" ||
               name == "input_i64" || name == "input_f64" ||
               name == "print" || name == "println" ||
               name == "malloc" || name == "free";
    }

    [[nodiscard]] ValueType builtin_type(const Expr& expression) const {
        const auto require_arguments = [&](const std::size_t expected) {
            if (expression.arguments.size() != expected) {
                fail(expression.token,
                     "builtin '" + expression.name + "' expects " +
                         std::to_string(expected) + " argument" +
                         (expected == 1 ? "" : "s"));
            }
        };

        if (expression.name == "input" ||
            expression.name == "input_text") {
            require_arguments(0);
            return ValueType::text;
        }
        if (expression.name == "input_i64") {
            require_arguments(0);
            return ValueType::i64;
        }
        if (expression.name == "input_f64") {
            require_arguments(0);
            return ValueType::f64;
        }
        if (expression.name == "malloc") {
            require_arguments(1);
            const auto size = semantic_type(*expression.arguments.front());
            if (size.pointer_depth != 0 || !size.class_name.empty() ||
                !is_integral(size.type)) {
                fail(expression.token,
                     "malloc size must be an integral value");
            }
            return ValueType::u64;
        }
        if (expression.name == "free") {
            require_arguments(1);
            const auto pointer =
                semantic_type(*expression.arguments.front());
            if (pointer.pointer_depth == 0) {
                fail(expression.token, "free expects a pointer");
            }
            return ValueType::i64;
        }
        if (expression.name == "print" || expression.name == "println") {
            require_arguments(1);
            const auto argument =
                semantic_type(*expression.arguments.front());
            if (argument.pointer_depth != 0) {
                fail(expression.token, "cannot print a pointer");
            }
            if (!argument.class_name.empty()) {
                fail(expression.token, "cannot print class object '" +
                                           argument.class_name + "'");
            }
            return ValueType::i64;
        }
        throw std::logic_error("invalid Concept builtin");
    }

    ValueType compile_builtin(const Expr& expression) {
        const auto result_type = builtin_type(expression);
        if (expression.name == "input" ||
            expression.name == "input_text") {
            emit(Op::input_text);
            return result_type;
        }
        if (expression.name == "input_i64") {
            emit(Op::input_i64);
            return result_type;
        }
        if (expression.name == "input_f64") {
            emit(Op::input_f64);
            return result_type;
        }
        if (expression.name == "malloc") {
            compile_malloc(expression, SemanticType{ValueType::u8, {}, 1});
            return ValueType::u64;
        }
        if (expression.name == "free") {
            static_cast<void>(
                compile_expression(*expression.arguments.front()));
            emit(Op::heap_free);
            return result_type;
        }
        const auto argument_type =
            compile_expression(*expression.arguments.front());
        emit(expression.name == "print" ? Op::print : Op::println);
        emit_type(argument_type);
        return result_type;
    }

    void compile_malloc(const Expr& expression,
                        SemanticType pointer_type) {
        static_cast<void>(builtin_type(expression));
        if (pointer_type.pointer_depth == 0) {
            fail(expression.token, "malloc requires a pointer destination");
        }
        compile_expression_as(*expression.arguments.front(), ValueType::u64);
        --pointer_type.pointer_depth;
        if (!pointer_type.class_name.empty() &&
            pointer_type.pointer_depth == 0) {
            fail(expression.token,
                 "malloc cannot construct a class object");
        }
        emit(Op::heap_alloc);
        emit_type(pointer_type.pointer_depth != 0 ||
                          !pointer_type.class_name.empty()
                      ? ValueType::u64
                      : pointer_type.type);
    }

    [[nodiscard]] ValueType expression_type(const Expr& expression) const {
        const auto result = semantic_type(expression);
        return result.pointer_depth != 0 || !result.class_name.empty()
                   ? ValueType::u64
                   : result.type;
    }

    [[nodiscard]] static bool is_comparison(const TokenKind operation) {
        return operation == TokenKind::equal ||
               operation == TokenKind::not_equal ||
               operation == TokenKind::less ||
               operation == TokenKind::less_equal ||
               operation == TokenKind::greater ||
               operation == TokenKind::greater_equal;
    }

    [[nodiscard]] ValueType comparison_operand_type(
        const Expr& expression, const ValueType left,
        const ValueType right) const {
        if (left == ValueType::boolean && right == ValueType::boolean) {
            if (expression.op != TokenKind::equal &&
                expression.op != TokenKind::not_equal) {
                fail(expression.token,
                     "bool supports only '==' and '!=' comparisons");
            }
            return ValueType::boolean;
        }
        if (left == ValueType::text && right == ValueType::text) {
            if (expression.op != TokenKind::equal &&
                expression.op != TokenKind::not_equal) {
                fail(expression.token,
                     "string supports only '==' and '!=' comparisons");
            }
            return ValueType::text;
        }
        return common_numeric_type(expression, left, right);
    }

    [[nodiscard]] ValueType common_numeric_type(
        const Expr& expression, const ValueType left,
        const ValueType right) const {
        if (!is_numeric(left) || !is_numeric(right)) {
            fail(expression.token, "operator requires numeric operands, got " +
                                       std::string(value_type_name(left)) +
                                       " and " +
                                       std::string(value_type_name(right)));
        }
        if (left == ValueType::f64 || right == ValueType::f64) {
            return ValueType::f64;
        }
        if (left == ValueType::f32 || right == ValueType::f32) {
            return ValueType::f32;
        }

        const auto left_width = integral_width(left);
        const auto right_width = integral_width(right);
        const bool left_signed = is_signed_integral(left);
        const bool right_signed = is_signed_integral(right);
        if (left_signed == right_signed) {
            return integral_type(left_width > right_width ? left_width
                                                          : right_width,
                                 left_signed);
        }

        const auto unsigned_width = left_signed ? right_width : left_width;
        const auto signed_width = left_signed ? left_width : right_width;
        if (unsigned_width >= signed_width) {
            return integral_type(unsigned_width, false);
        }
        return integral_type(signed_width, true);
    }

    [[nodiscard]] static unsigned integral_width(const ValueType type) {
        switch (type) {
        case ValueType::i8:
        case ValueType::u8:
            return 8;
        case ValueType::i16:
        case ValueType::u16:
            return 16;
        case ValueType::i32:
        case ValueType::u32:
            return 32;
        case ValueType::i64:
        case ValueType::u64:
            return 64;
        default:
            throw std::logic_error("non-integral type has no bit width");
        }
    }

    [[nodiscard]] static ValueType integral_type(const unsigned width,
                                                 const bool is_signed) {
        if (is_signed) {
            switch (width) {
            case 8:
                return ValueType::i8;
            case 16:
                return ValueType::i16;
            case 32:
                return ValueType::i32;
            default:
                return ValueType::i64;
            }
        }
        switch (width) {
        case 8:
            return ValueType::u8;
        case 16:
            return ValueType::u16;
        case 32:
            return ValueType::u32;
        default:
            return ValueType::u64;
        }
    }

    [[nodiscard]] Op binary_op(const Expr& expression) const {
        switch (expression.op) {
        case TokenKind::plus:
            return Op::add;
        case TokenKind::minus:
            return Op::subtract;
        case TokenKind::star:
            return Op::multiply;
        case TokenKind::slash:
            return Op::divide;
        case TokenKind::percent:
            return Op::modulo;
        case TokenKind::equal:
            return Op::equal;
        case TokenKind::not_equal:
            return Op::not_equal;
        case TokenKind::less:
            return Op::less;
        case TokenKind::less_equal:
            return Op::less_equal;
        case TokenKind::greater:
            return Op::greater;
        case TokenKind::greater_equal:
            return Op::greater_equal;
        default:
            fail(expression.token, "unsupported binary operator");
        }
    }

    const LocalInfo& lookup_local(const Token& token,
                                  const std::string& name) const {
        const auto local = locals_.find(name);
        if (local == locals_.end()) {
            fail(token, "unknown variable '" + name + "'");
        }
        return local->second;
    }

    const ClassInfo& lookup_class(const Token& token,
                                  const std::string& name) const {
        const auto declaration = class_info_.find(name);
        if (declaration == class_info_.end()) {
            fail(token, "unknown class '" + name + "'");
        }
        return declaration->second;
    }

    const FieldInfo& lookup_field(const Token& token,
                                  const std::string& class_name,
                                  const std::string& name) const {
        const auto& declaration = lookup_class(token, class_name);
        const auto field = declaration.fields.find(name);
        if (field == declaration.fields.end()) {
            fail(token, "class '" + class_name + "' has no field '" + name +
                            "'");
        }
        return field->second;
    }

    const std::string& lookup_method(const Token& token,
                                     const std::string& class_name,
                                     const std::string& name) const {
        const auto& declaration = lookup_class(token, class_name);
        const auto method = declaration.methods.find(name);
        if (method == declaration.methods.end()) {
            fail(token, "class '" + class_name + "' has no method '" + name +
                            "'");
        }
        return method->second;
    }

    bool is_native_socket_class(const std::string& class_name) const {
        const auto declaration = class_info_.find(class_name);
        return declaration != class_info_.end() &&
               declaration->second.native_socket;
    }

    ValueType socket_method_type(const Expr& expression) const {
        const auto require_arguments = [&](const std::size_t expected) {
            if (expression.arguments.size() != expected) {
                fail(expression.token,
                     "std::Socket." + expression.name + " expects " +
                         std::to_string(expected) + " argument" +
                         (expected == 1 ? "" : "s"));
            }
        };

        if (expression.name == "connect" || expression.name == "bind") {
            require_arguments(2);
            return ValueType::boolean;
        }
        if (expression.name == "listen") {
            require_arguments(1);
            return ValueType::boolean;
        }
        if (expression.name == "accept") {
            require_arguments(0);
            return ValueType::u64;
        }
        if (expression.name == "send") {
            require_arguments(1);
            return ValueType::i64;
        }
        if (expression.name == "recv") {
            require_arguments(0);
            return ValueType::text;
        }
        if (expression.name == "close") {
            require_arguments(0);
            return ValueType::boolean;
        }
        if (expression.name == "valid") {
            require_arguments(0);
            return ValueType::boolean;
        }
        fail(expression.token, "std::Socket has no method '" +
                                   expression.name + "'");
    }

    ValueType compile_socket_method(const Expr& expression) {
        const auto result_type = socket_method_type(expression);
        compile_object_expression_as(*expression.left, "std::Socket");
        if (expression.name == "connect" || expression.name == "bind") {
            compile_expression_as(*expression.arguments[0], ValueType::text);
            compile_expression_as(*expression.arguments[1], ValueType::u16);
            emit(expression.name == "connect" ? Op::socket_connect
                                               : Op::socket_bind);
        } else if (expression.name == "listen") {
            compile_expression_as(*expression.arguments[0], ValueType::i32);
            emit(Op::socket_listen);
        } else if (expression.name == "accept") {
            emit(Op::socket_accept);
        } else if (expression.name == "send") {
            compile_expression_as(*expression.arguments[0], ValueType::text);
            emit(Op::socket_send);
        } else if (expression.name == "recv") {
            emit(Op::socket_receive);
        } else if (expression.name == "valid") {
            emit(Op::push_bits);
            emit_u64(std::numeric_limits<std::uint64_t>::max());
            emit(Op::not_equal);
            emit_type(ValueType::u64);
        } else {
            emit(Op::socket_close);
        }
        return result_type;
    }

    std::string expression_class(const Expr& expression) const {
        const auto result = semantic_type(expression);
        return result.pointer_depth == 0 ? result.class_name : std::string{};
    }

    void compile_object_expression_as(const Expr& expression,
                                      const std::string& expected_class) {
        const auto actual_class = expression_class(expression);
        if (actual_class.empty()) {
            fail(expression.token, "expected object of class '" +
                                       expected_class + "'");
        }
        if (actual_class != expected_class) {
            fail(expression.token, "cannot use object of class '" +
                                       actual_class + "' as '" +
                                       expected_class + "'");
        }
        static_cast<void>(compile_expression(expression));
    }

    void emit_new_object(const std::string& class_name) {
        const auto& declaration = class_info_.at(class_name);
        if (declaration.native_socket) {
            emit(Op::socket_open);
            return;
        }
        emit(Op::new_object);
        emit_u16(declaration.field_count);
    }

    const FunctionInfo& lookup_function(const Token& token,
                                        const std::string& name) const {
        const auto function = function_info_.find(name);
        if (function == function_info_.end()) {
            fail(token, "unknown function '" + name + "'");
        }
        return function->second;
    }

    void resolve_calls() {
        for (const auto& patch : call_patches_) {
            const auto function = function_info_.find(patch.name);
            if (function == function_info_.end()) {
                fail(patch.token, "unknown function '" + patch.name + "'");
            }
            patch_u32(patch.target_offset, function->second.entry);
            patch_u32(patch.locals_offset, function->second.locals);
        }
    }

    void emit_default_value(const ValueType type) {
        if (type == ValueType::text) {
            emit(Op::push_text);
            emit_u32(add_string(""));
            return;
        }
        emit(Op::push_bits);
        emit_u64(0);
    }

    std::uint64_t next_obfuscation_random() {
        obfuscation_state_ += 0x9e3779b97f4a7c15ULL;
        return mix_seed(obfuscation_state_);
    }

    void emit_obfuscation() {
        obfuscation_credit_ += current_complexity_;
        while (obfuscation_credit_ >= 25) {
            emit_obfuscation_layer();
            obfuscation_credit_ -= 25;
        }
    }

    void emit_obfuscation_layer() {
        emit(Op::jump);
        const auto predicate_target = reserve_u32();

        const auto dead_target = checked_offset();
        emit(Op::push_bits);
        emit_u64(next_obfuscation_random());
        emit(Op::push_bits);
        emit_u64(next_obfuscation_random());
        emit(Op::multiply);
        emit_type(ValueType::u64);
        emit(Op::pop);
        emit(Op::push_bits);
        emit_u64(next_obfuscation_random());
        emit(Op::negate);
        emit_type(ValueType::i64);
        emit(Op::pop);
        emit(Op::jump);
        const auto dead_exit = reserve_u32();

        patch_u32(predicate_target, checked_offset());
        const auto left = next_obfuscation_random();
        const auto right = next_obfuscation_random();
        emit(Op::push_bits);
        emit_u64(left);
        emit(Op::push_bits);
        emit_u64(right);
        emit(Op::add);
        emit_type(ValueType::u64);
        emit(Op::push_bits);
        emit_u64(left + right);
        emit(Op::equal);
        emit_type(ValueType::u64);
        emit(Op::jump_if_false);
        emit_u32(dead_target);
        emit(Op::jump);
        const auto live_exit = reserve_u32();

        const auto live_target = checked_offset();
        patch_u32(dead_exit, live_target);
        patch_u32(live_exit, live_target);
    }

    std::uint32_t add_string(const std::string& value) {
        if (strings_.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw CompileError("too many string constants");
        }
        strings_.push_back(value);
        return static_cast<std::uint32_t>(strings_.size() - 1);
    }

    void emit(const Op op) { code_.push_back(static_cast<std::uint8_t>(op)); }

    void emit_type(const ValueType type) {
        code_.push_back(static_cast<std::uint8_t>(type));
    }

    void emit_conversion(const ValueType source, const ValueType target,
                         const Token& token) {
        if (source == target) {
            return;
        }
        if (source == ValueType::text || target == ValueType::text) {
            fail(token, "cannot convert " +
                            std::string(value_type_name(source)) + " to " +
                            std::string(value_type_name(target)));
        }
        emit(Op::convert);
        emit_type(source);
        emit_type(target);
    }

    void emit_u16(const std::uint16_t value) {
        code_.push_back(static_cast<std::uint8_t>(value));
        code_.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void emit_u32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            code_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void emit_u64(const std::uint64_t bits) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            code_.push_back(static_cast<std::uint8_t>(bits >> shift));
        }
    }

    std::size_t reserve_u32() {
        const auto offset = code_.size();
        emit_u32(0);
        return offset;
    }

    void patch_u32(const std::size_t offset, const std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) {
            code_[offset + index] =
                static_cast<std::uint8_t>(value >> (index * 8));
        }
    }

    [[nodiscard]] std::uint32_t checked_offset() const {
        if (code_.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw CompileError("generated bytecode is too large");
        }
        return static_cast<std::uint32_t>(code_.size());
    }
};

} // namespace

Bytecode compile(const std::string_view source, const std::string_view filename,
                 const std::uint32_t vm_count,
                 const std::string_view module_root) {
    if (vm_count == 0 || vm_count > 64) {
        throw CompileError("VM count must be between 1 and 64");
    }
    auto program = ModuleLoader(filename, module_root).load(source);
    auto bytecode =
        CodeGenerator(program.classes, program.functions, filename,
                      program.has_std)
            .generate();
    bytecode.vm_seeds.reserve(vm_count);
    for (std::uint32_t index = 0; index < vm_count; ++index) {
        bytecode.vm_seeds.push_back(fresh_opcode_seed());
    }
    fill_random_bytes(bytecode.string_key);
    fill_random_bytes(bytecode.string_nonce);
    return bytecode;
}

} // namespace cpt
