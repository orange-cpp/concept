#include "concept/compiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
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
    semicolon,
    comma,
    arrow,
    assign,
    plus,
    minus,
    star,
    slash,
    percent,
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
    std::string_view text;
    std::size_t line{};
    std::size_t column{};
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
            while (!at_end() && is_identifier_part(peek())) {
                advance();
            }
            const auto text = source_.substr(start, position_ - start);
            TokenKind kind = TokenKind::identifier;
            if (text == "fn") {
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
        case ';':
            return make_token(TokenKind::semicolon, start, 1, line, column);
        case ',':
            return make_token(TokenKind::comma, start, 1, line, column);
        case '+':
            return make_token(TokenKind::plus, start, 1, line, column);
        case '*':
            return make_token(TokenKind::star, start, 1, line, column);
        case '/':
            return make_token(TokenKind::slash, start, 1, line, column);
        case '%':
            return make_token(TokenKind::percent, start, 1, line, column);
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
        return {kind, source_.substr(start, length), line, column};
    }

    [[noreturn]] void fail(const std::size_t line, const std::size_t column,
                           const std::string& message) const {
        throw CompileError(std::string(filename_) + ':' + std::to_string(line) +
                           ':' + std::to_string(column) + ": " + message);
    }
};

struct Expr {
    enum class Kind { literal, variable, call, cast, unary, binary } kind{};
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
        expression,
        return_value,
        if_statement,
        while_statement,
    } kind{};
    Token token;
    std::string name;
    ValueType value_type{ValueType::i64};
    std::unique_ptr<Expr> expression;
    std::unique_ptr<Stmt> first;
    std::unique_ptr<Stmt> second;
    std::vector<std::unique_ptr<Stmt>> statements;
};

struct Function {
    Token name;
    ValueType return_type{ValueType::i64};
    std::uint8_t complexity{};
    std::vector<std::unique_ptr<Stmt>> body;
};

class Parser {
public:
    Parser(const std::string_view source, const std::string_view filename)
        : lexer_(source, filename), filename_(filename), current_(lexer_.next()),
          next_(lexer_.next()) {}

    std::vector<Function> parse_program() {
        std::vector<Function> functions;
        while (current_.kind != TokenKind::end) {
            functions.push_back(parse_function());
        }
        if (functions.empty()) {
            fail(current_, "expected at least one function");
        }
        return functions;
    }

private:
    Lexer lexer_;
    std::string_view filename_;
    Token current_;
    Token next_;

    void advance() {
        current_ = next_;
        next_ = lexer_.next();
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
        throw CompileError(std::string(filename_) + ':' +
                           std::to_string(token.line) + ':' +
                           std::to_string(token.column) + ": " +
                           std::string(message));
    }

    Function parse_function() {
        Function function;
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
        const auto return_type =
            consume(TokenKind::type_kw, "expected function return type");
        function.return_type = value_type_from_name(return_type.text);
        function.body = parse_block_contents();
        return function;
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
        if (current_.kind == TokenKind::type_kw) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::variable;
            statement->token = current_;
            statement->value_type = value_type_from_name(current_.text);
            advance();
            const auto name =
                consume(TokenKind::identifier, "expected variable name");
            statement->name = name.text;
            if (match(TokenKind::assign)) {
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
        if (current_.kind == TokenKind::identifier &&
            next_.kind == TokenKind::assign) {
            auto statement = std::make_unique<Stmt>();
            statement->kind = Stmt::Kind::assign;
            statement->token = current_;
            statement->name = current_.text;
            advance();
            advance();
            statement->expression = parse_expression();
            consume(TokenKind::semicolon, "expected ';' after assignment");
            return statement;
        }

        auto statement = std::make_unique<Stmt>();
        statement->kind = Stmt::Kind::expression;
        statement->token = current_;
        statement->expression = parse_expression();
        consume(TokenKind::semicolon, "expected ';' after expression");
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
            current_.kind == TokenKind::bang) {
            auto expression = std::make_unique<Expr>();
            expression->kind = Expr::Kind::unary;
            expression->token = current_;
            expression->op = current_.kind;
            advance();
            expression->right = parse_unary();
            return expression;
        }
        return parse_primary();
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
            const auto* begin = current_.text.data();
            const auto* end = begin + current_.text.size();
            const auto result = std::from_chars(begin, end, expression->bits);
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

struct FunctionInfo {
    std::uint32_t entry{};
    std::uint32_t locals{};
    ValueType return_type{ValueType::i64};
};

struct LocalInfo {
    std::uint16_t index{};
    ValueType type{ValueType::i64};
};

struct CallPatch {
    std::size_t target_offset{};
    std::size_t locals_offset{};
    Token token;
    std::string name;
};

class CodeGenerator {
public:
    CodeGenerator(std::vector<Function>& functions, const std::string_view filename)
        : functions_(functions), filename_(filename) {}

    Bytecode generate() {
        register_functions();
        for (const auto& function : functions_) {
            compile_function(function);
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
    std::vector<Function>& functions_;
    std::string_view filename_;
    std::vector<std::uint8_t> code_;
    std::unordered_map<std::string, FunctionInfo> function_info_;
    std::unordered_map<std::string, LocalInfo> locals_;
    std::vector<CallPatch> call_patches_;
    std::vector<std::string> strings_;
    ValueType current_return_type_{ValueType::i64};
    std::uint8_t current_complexity_{};
    std::uint32_t obfuscation_credit_{};
    std::uint64_t obfuscation_state_{fresh_opcode_seed()};

    [[noreturn]] void fail(const Token& token,
                           const std::string& message) const {
        throw CompileError(std::string(filename_) + ':' +
                           std::to_string(token.line) + ':' +
                           std::to_string(token.column) + ": " + message);
    }

    void register_functions() {
        for (const auto& function : functions_) {
            const std::string name(function.name.text);
            if (is_builtin_name(name)) {
                fail(function.name,
                     "function name '" + name + "' is reserved by the VM");
            }
            if (!function_info_
                     .emplace(name,
                              FunctionInfo{0, 0, function.return_type})
                     .second) {
                fail(function.name, "duplicate function '" + name + "'");
            }
        }
    }

    void compile_function(const Function& function) {
        auto& info = function_info_.at(std::string(function.name.text));
        info.entry = checked_offset();
        current_return_type_ = function.return_type;
        current_complexity_ = function.complexity;
        obfuscation_credit_ = 0;
        locals_.clear();
        if (current_complexity_ != 0) {
            emit_obfuscation_layer();
        }
        for (const auto& statement : function.body) {
            compile_statement(*statement);
        }

        emit_obfuscation();
        emit_default_value(function.return_type);
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
            const auto index = static_cast<std::uint16_t>(locals_.size());
            locals_.emplace(statement.name,
                            LocalInfo{index, statement.value_type});
            if (statement.expression) {
                compile_expression_as(*statement.expression,
                                      statement.value_type);
            } else {
                emit_default_value(statement.value_type);
            }
            emit(Op::store);
            emit_u16(index);
            return;
        }
        case Stmt::Kind::assign: {
            const auto& local = lookup_local(statement.token, statement.name);
            compile_expression_as(*statement.expression, local.type);
            emit(Op::store);
            emit_u16(local.index);
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
            return local.type;
        }
        case Expr::Kind::call: {
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
            const auto source_type = compile_expression(*expression.right);
            emit_conversion(source_type, expression.value_type,
                            expression.token);
            return expression.value_type;
        }
        case Expr::Kind::unary: {
            const auto operand_type = expression_type(*expression.right);
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
        const auto source_type = compile_expression(expression);
        emit_conversion(source_type, target_type, expression.token);
    }

    [[nodiscard]] static bool is_builtin_name(const std::string_view name) {
        return name == "input" || name == "input_text" ||
               name == "input_i64" || name == "input_f64" ||
               name == "print" || name == "println";
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
        if (expression.name == "print" || expression.name == "println") {
            require_arguments(1);
            static_cast<void>(expression_type(*expression.arguments.front()));
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

        const auto argument_type =
            compile_expression(*expression.arguments.front());
        emit(expression.name == "print" ? Op::print : Op::println);
        emit_type(argument_type);
        return result_type;
    }

    [[nodiscard]] ValueType expression_type(const Expr& expression) const {
        switch (expression.kind) {
        case Expr::Kind::literal:
        case Expr::Kind::cast:
            return expression.value_type;
        case Expr::Kind::variable:
            return lookup_local(expression.token, expression.name).type;
        case Expr::Kind::call:
            if (is_builtin_name(expression.name)) {
                return builtin_type(expression);
            }
            if (!expression.arguments.empty()) {
                fail(expression.token,
                     "user-defined function parameters are not supported yet");
            }
            return lookup_function(expression.token, expression.name).return_type;
        case Expr::Kind::unary: {
            const auto operand_type = expression_type(*expression.right);
            if (expression.op == TokenKind::bang) {
                return ValueType::boolean;
            }
            if (!is_numeric(operand_type)) {
                fail(expression.token, "unary '-' requires a numeric operand");
            }
            return operand_type;
        }
        case Expr::Kind::binary: {
            const auto left_type = expression_type(*expression.left);
            const auto right_type = expression_type(*expression.right);
            if (is_comparison(expression.op)) {
                static_cast<void>(comparison_operand_type(
                    expression, left_type, right_type));
                return ValueType::boolean;
            }
            const auto type =
                common_numeric_type(expression, left_type, right_type);
            if (expression.op == TokenKind::percent && is_floating(type)) {
                fail(expression.token,
                     "operator '%' requires integral operands");
            }
            return type;
        }
        }
        throw std::logic_error("invalid expression kind");
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
                 const std::uint32_t vm_count) {
    if (vm_count == 0 || vm_count > 64) {
        throw CompileError("VM count must be between 1 and 64");
    }
    Parser parser(source, filename);
    auto functions = parser.parse_program();
    auto bytecode = CodeGenerator(functions, filename).generate();
    bytecode.vm_seeds.reserve(vm_count);
    for (std::uint32_t index = 0; index < vm_count; ++index) {
        bytecode.vm_seeds.push_back(fresh_opcode_seed());
    }
    fill_random_bytes(bytecode.string_key);
    fill_random_bytes(bytecode.string_nonce);
    return bytecode;
}

} // namespace cpt
