#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"
#include "table.hpp"
#include "token.hpp"
#include "options.hpp"
#include "util.hpp"

namespace cuke::internal
{

class parser
{
 public:
  [[nodiscard]] const ast::gherkin_document& head() const noexcept;
  [[nodiscard]] bool error() const noexcept;

  void parse_from_file(const cuke::feature_file& file);
  void parse_from_file(std::string_view filepath);
  void parse_script(std::string_view script);

  const ast::scenario_node* get_scenario_from_line(std::size_t line) const;
  void for_each_scenario(ast::node_visitor& visitor) const;

 private:
  void parse_impl(std::string_view script, std::string_view filename);

  [[nodiscard]] std::pair<std::string, std::string> parse_keyword_and_name(
      bool remove_colon);
  [[nodiscard]] std::vector<std::string> parse_tags();
  [[nodiscard]] std::vector<std::string> parse_doc_string();
  [[nodiscard]] std::vector<std::string> doc_string_to_vector(
      std::string_view s);
  [[nodiscard]] std::string trim(const std::string& str);
  [[nodiscard]] std::size_t advance_to_cell_end();
  [[nodiscard]] cuke::value parse_cell(bool remove_quotes_from_strings);
  [[nodiscard]] cuke::value_array parse_row(bool remove_quotes_from_strings);
  [[nodiscard]] std::pair<cuke::table, std::vector<std::size_t>> parse_table(
      bool remove_quotes_from_strings);
  [[nodiscard]] std::vector<ast::step_node> parse_steps();
  [[nodiscard]] ast::example_node parse_example(
      std::vector<std::string>&& tags);
  [[nodiscard]] std::unique_ptr<ast::scenario_outline_node>
  make_scenario_outline(std::vector<std::string>&& tags,
                        const std::optional<ast::rule_node>& rule);
  [[nodiscard]] std::unique_ptr<ast::scenario_node> make_scenario(
      std::vector<std::string>&& tags,
      const std::optional<ast::rule_node>& rule,
      const ast::background_node* background, const std::string& id_prefix);
  [[nodiscard]] std::optional<ast::rule_node> parse_rule();
  [[nodiscard]] std::vector<std::unique_ptr<ast::node>> parse_scenarios(
      const std::vector<std::string>& feature_tags,
      const ast::background_node* background, const std::string& feature_id);
  [[nodiscard]] std::unique_ptr<ast::background_node> parse_background();
  [[nodiscard]] ast::feature_node parse_feature();

  template <typename... Ts>
  [[nodiscard]] std::vector<std::string> parse_description(
      Ts&&... terminators)
  {
    auto& lex = *m_lexer;
    std::vector<std::string> lines;
    while (!lex.check(std::forward<Ts>(terminators)...))
    {
      token begin = lex.current();
      lex.advance_to(token_type::linebreak);
      token end = lex.previous();
      lex.advance();

      auto is_not_comment_or_empty = [&begin, &end]()
      { return begin.line > end.line; };
      lines.push_back(is_not_comment_or_empty() ? create_string(end, begin)
                                                 : create_string(begin, end));
    }
    return lines;
  }

 private:
  ast::gherkin_document m_head;
  bool m_error{false};
  lexer* m_lexer{nullptr};
};

}  // namespace cuke::internal
