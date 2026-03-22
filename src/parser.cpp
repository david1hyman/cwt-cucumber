#include "parser.hpp"

#include <algorithm>
#include <format>
#include <ranges>

#include "log.hpp"

namespace cuke::internal
{

const ast::gherkin_document& parser::head() const noexcept { return m_head; }

bool parser::error() const noexcept { return m_error; }

void parser::parse_from_file(const cuke::feature_file& file)
{
  parse_from_file(file.path);
}

void parser::parse_from_file(std::string_view filepath)
{
  const std::string script = read_file(filepath);
  if (script.empty())
  {
    log::error("Error: File not found '", filepath, "'", log::new_line);
    return;
  }
  parse_impl(script, filepath);
}

void parser::parse_script(std::string_view script)
{
  parse_impl(script, "<no file>");
}

const ast::scenario_node* parser::get_scenario_from_line(
    std::size_t line) const
{
  for (const auto& n : m_head.feature().scenarios())
  {
    if (n->type() == ast::node_type::scenario && n->line() == line)
    {
      return static_cast<const ast::scenario_node*>(n.get());
    }
    else if (n->type() == ast::node_type::scenario_outline)
    {
      for (const auto& scenario :
           static_cast<const ast::scenario_outline_node*>(n.get())
               ->concrete_scenarios())
      {
        if (scenario.line() == line)
        {
          return &scenario;
        }
      }
    }
  }
  return nullptr;
}

void parser::for_each_scenario(ast::node_visitor& visitor) const
{
  visitor.visit(m_head.feature());
  for (const auto& n : m_head.feature().scenarios())
  {
    n->accept(visitor);
  }
}

void parser::parse_impl(std::string_view script, std::string_view filename)
{
  m_error = false;
  lexer lex(script, filename);
  m_lexer = &lex;
  lex.advance();
  lex.skip_linebreaks();
  m_head.set_feature(parse_feature());
  if (lex.error())
  {
    log::error("Error while parsing script", log::new_line);
    m_error = true;
    m_head.clear();
  }
  m_lexer = nullptr;
}

std::pair<std::string, std::string> parser::parse_keyword_and_name(
    bool remove_colon)
{
  auto& lex = *m_lexer;
  std::string key = create_string(lex.current().value, remove_colon ? 1 : 0);
  lex.advance();

  auto make_name = [](lexer& lex)
  {
    token begin = lex.current();
    lex.advance_to(token_type::linebreak, token_type::eof);
    token end = lex.previous();
    return create_string(begin, end);
  };

  std::string name = lex.current().type == token_type::linebreak
                         ? std::string("")
                         : make_name(lex);
  lex.advance();
  lex.skip_linebreaks();
  return std::make_pair(key, name);
}

std::vector<std::string> parser::parse_tags()
{
  auto& lex = *m_lexer;
  std::vector<std::string> tags;
  while (lex.check(token_type::tag))
  {
    tags.push_back(create_string(lex.current().value));
    lex.advance();
  }
  lex.skip_linebreaks();
  return tags;
}

std::string parser::trim(const std::string& str)
{
  auto is_space = [](char c)
  { return std::isspace(static_cast<unsigned char>(c)); };
  auto start = std::find_if_not(str.begin(), str.end(), is_space);
  auto end = std::find_if_not(str.rbegin(), str.rend(), is_space).base();
  return (start < end ? std::string(start, end) : "");
}

std::vector<std::string> parser::doc_string_to_vector(
    const std::string_view s)
{
  auto lines_view = s | std::ranges::views::split('\n');
  std::vector<std::string> lines;
  for (auto&& line : lines_view | std::views::drop(1))
  {
    lines.push_back(trim(std::string(line.begin(), line.end())));
  }
  lines.pop_back();
  return lines;
}

std::vector<std::string> parser::parse_doc_string()
{
  auto& lex = *m_lexer;
  if (lex.match(token_type::doc_string))
  {
    return doc_string_to_vector(lex.previous().value);
  }
  else
  {
    return {};
  }
}

std::size_t parser::advance_to_cell_end()
{
  auto& lex = *m_lexer;
  std::size_t count = 0;
  while (!lex.check(token_type::vertical))
  {
    ++count;
    lex.advance();
    if (lex.check(token_type::eof, token_type::linebreak))
    {
      lex.error_at(lex.current(), "Expect '|' after value in cell");
      return 0;
    }
  }
  return count;
}

cuke::value parser::parse_cell(bool remove_quotes_from_strings)
{
  auto& lex = *m_lexer;
  token begin = lex.current();

  std::size_t count = advance_to_cell_end();

  if (count == 0)
  {
    lex.error_at(lex.current(), "Expect value in table cell");
    return {};
  }

  cuke::value v(create_string(begin, lex.previous()));
  if (remove_quotes_from_strings)
  {
    v.emplace_or_replace(remove_quotes(v.to_string()));
  }
  return v;
}

cuke::value_array parser::parse_row(bool remove_quotes_from_strings)
{
  auto& lex = *m_lexer;
  cuke::value_array v;
  while (!(lex.match(token_type::linebreak) || lex.match(token_type::eof)))
  {
    v.push_back(parse_cell(remove_quotes_from_strings));
    if (!lex.match(token_type::vertical))
    {
      v.clear();
      lex.error_at(lex.current(), "Expect '|' after value in data table");
      break;
    }
  }
  return v;
}

std::pair<cuke::table, std::vector<std::size_t>> parser::parse_table(
    bool remove_quotes_from_strings)
{
  auto& lex = *m_lexer;
  if (!lex.match(token_type::vertical))
  {
    return {};
  }
  std::vector<std::size_t> lines;
  lines.push_back(lex.current().line);
  cuke::table t(parse_row(remove_quotes_from_strings));
  lex.skip_linebreaks();
  while (lex.match(token_type::vertical))
  {
    lines.push_back(lex.current().line);
    if (!t.append_row(parse_row(remove_quotes_from_strings)) || lex.error())
    {
      lex.error_at(lex.current(), "Different row lengths in data table");
      return {};
    }
    lex.skip_linebreaks();
  }
  return std::make_pair(std::move(t), std::move(lines));
}

std::vector<ast::step_node> parser::parse_steps()
{
  auto& lex = *m_lexer;
  std::vector<ast::step_node> steps;
  while (lex.check(token_type::step))
  {
    const std::size_t line = lex.current().line;
    auto [key, name] = parse_keyword_and_name(false);
    std::vector<std::string> doc_string = parse_doc_string();
    auto [data_table, line_table_begin] = parse_table(true);

    steps.push_back(ast::step_node(std::move(key), std::move(name),
                                   lex.filepath(), line, std::move(doc_string),
                                   std::move(data_table)));

    lex.skip_linebreaks();
  }
  return steps;
}

ast::example_node parser::parse_example(std::vector<std::string>&& tags)
{
  auto& lex = *m_lexer;
  const std::size_t line = lex.current().line;
  auto [keyword, name] = parse_keyword_and_name(true);
  auto description =
      parse_description(token_type::vertical, token_type::eof);
  auto [t, lines_from_table_rows] = parse_table(false);
  return ast::example_node(std::move(keyword), std::move(name),
                           lex.filepath(), line, std::move(tags),
                           std::move(description), std::move(t),
                           std::move(lines_from_table_rows));
}

std::unique_ptr<ast::scenario_outline_node> parser::make_scenario_outline(
    std::vector<std::string>&& tags,
    const std::optional<ast::rule_node>& rule)
{
  auto& lex = *m_lexer;
  const std::size_t line = lex.current().line;
  auto [key, name] = parse_keyword_and_name(true);
  auto description = parse_description(token_type::step, token_type::eof);
  auto steps = parse_steps();

  return std::make_unique<ast::scenario_outline_node>(
      std::move(key), std::move(name), lex.filepath(), line, std::move(steps),
      std::move(tags), std::move(description), rule);
}

std::unique_ptr<ast::scenario_node> parser::make_scenario(
    std::vector<std::string>&& tags,
    const std::optional<ast::rule_node>& rule,
    const ast::background_node* background, const std::string& id_prefix)
{
  auto& lex = *m_lexer;
  const std::size_t line = lex.current().line;
  auto [key, name] = parse_keyword_and_name(true);
  auto description = parse_description(token_type::step, token_type::eof);
  auto steps = parse_steps();
  return std::make_unique<ast::scenario_node>(
      std::move(key), std::move(name), lex.filepath(), line, std::move(steps),
      std::move(tags), std::move(description), rule, background, id_prefix);
}

std::optional<ast::rule_node> parser::parse_rule()
{
  auto& lex = *m_lexer;
  if (lex.check(token_type::rule))
  {
    const std::size_t line = lex.current().line;
    auto [key, name] = parse_keyword_and_name(true);
    auto description = parse_description(token_type::scenario_outline,
                                         token_type::scenario, token_type::tag,
                                         token_type::eof);

    return ast::rule_node(std::move(key), std::move(name), lex.filepath(),
                          line, std::move(description));
  }

  return std::nullopt;
}

std::vector<std::unique_ptr<ast::node>> parser::parse_scenarios(
    const std::vector<std::string>& feature_tags,
    const ast::background_node* background, const std::string& feature_id)
{
  auto& lex = *m_lexer;
  std::vector<std::unique_ptr<ast::node>> scenarios;
  std::optional<ast::rule_node> current_rule = std::nullopt;

  while (!lex.error() && !lex.check(token_type::eof))
  {
    current_rule = parse_rule();

    const std::string id_prefix =
        current_rule.has_value()
            ? std::format("{};{}", feature_id, current_rule->name())
            : feature_id;

    auto tags = parse_tags();
    for (const auto& t : feature_tags)
    {
      if (std::find(tags.begin(), tags.end(), t) == tags.end())
      {
        tags.push_back(t);
      }
    }

    if (lex.check(token_type::scenario))
    {
      scenarios.push_back(make_scenario(std::move(tags), current_rule,
                                        background, id_prefix));
    }
    else if (lex.check(token_type::scenario_outline))
    {
      scenarios.push_back(
          make_scenario_outline(std::move(tags), current_rule));
    }
    else if (lex.check(token_type::examples) &&
             scenarios.back()->type() == ast::node_type::scenario_outline)
    {
      static_cast<ast::scenario_outline_node&>(*scenarios.back())
          .push_example(parse_example(std::move(tags)), background, id_prefix);
    }
    else
    {
      lex.error_at(lex.current(), "Expect Tags, Scenario or Scenario Outline");
      break;
    }
    lex.skip_linebreaks();
  }
  return std::move(scenarios);
}

std::unique_ptr<ast::background_node> parser::parse_background()
{
  auto& lex = *m_lexer;
  if (lex.check(token_type::background))
  {
    const std::size_t line = lex.current().line;
    auto [key, name] = parse_keyword_and_name(true);
    auto description =
        parse_description(token_type::step, token_type::eof);
    auto steps = parse_steps();
    return std::make_unique<ast::background_node>(
        std::move(key), std::move(name), lex.filepath(), line,
        std::move(steps), std::move(description));
  }
  return {};
}

ast::feature_node parser::parse_feature()
{
  auto& lex = *m_lexer;
  auto tags = parse_tags();
  if (!lex.check(token_type::feature))
  {
    lex.error_at(lex.current(), "Expect FeatureLine");
    return ast::feature_node{};
  }
  const std::size_t line = lex.current().line;
  auto [key, name] = parse_keyword_and_name(true);
  auto description = parse_description(
      token_type::scenario, token_type::scenario_outline, token_type::tag,
      token_type::background, token_type::rule, token_type::eof);
  lex.skip_linebreaks();
  auto background = parse_background();
  auto scenarios = parse_scenarios(tags, background.get(), name);

  return ast::feature_node(std::move(key), std::move(name), lex.filepath(),
                           line, std::move(scenarios), std::move(background),
                           std::move(tags), std::move(description));
}

}  // namespace cuke::internal
