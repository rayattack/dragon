-- Register Dragon tree-sitter parser (Neovim only)
if vim.fn.has("nvim-0.9") == 1 then
  vim.filetype.add({
    extension = {
      dr = "dragon",
    },
  })

  local ok, parsers = pcall(require, "nvim-treesitter.parsers")
  if ok then
    local parser_config = parsers.get_parser_configs()
    if not parser_config.dragon then
      parser_config.dragon = {
        install_info = {
          url = "https://github.com/tbhi/tree-sitter-dragon",
          files = { "src/parser.c" },
          generate_requires_npm = false,
          requires_generate_from_grammar = false,
        },
        filetype = "dragon",
      }
    end
  end
end
