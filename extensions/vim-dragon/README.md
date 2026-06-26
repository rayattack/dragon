# vim-dragon

Vim and Neovim support for the [Dragon](https://github.com/tbhi/dragon) programming language.

## Features

- **Syntax highlighting** (Tree-sitter for Neovim, regex for Vim)
- **Filetype detection** for `.dr` files
- **Comment toggling** (`gcc` in Neovim, works with commentary.vim in Vim)
- **Indentation** settings (4 spaces)
- **Tree-sitter queries** for highlights and indentation (Neovim)

## Installation

### lazy.nvim (Neovim)

```lua
{
  "tbhi/vim-dragon",
  ft = "dragon",
  dependencies = { "nvim-treesitter/nvim-treesitter" },
  config = function()
    -- Parser is auto-registered; just install it:
    vim.cmd("TSInstall dragon")
  end,
}
```

After installing, run `:TSInstall dragon` once to compile the Tree-sitter parser.

### packer.nvim (Neovim)

```lua
use {
  "tbhi/vim-dragon",
  ft = "dragon",
  requires = { "nvim-treesitter/nvim-treesitter" },
  config = function()
    vim.cmd("TSInstall dragon")
  end,
}
```

### vim-plug (Vim/Neovim)

```vim
Plug 'tbhi/vim-dragon'
```

### Manual installation

Clone into your Vim/Neovim packages directory:

```bash
# Neovim
git clone https://github.com/tbhi/vim-dragon ~/.local/share/nvim/site/pack/plugins/start/vim-dragon

# Vim
git clone https://github.com/tbhi/vim-dragon ~/.vim/pack/plugins/start/vim-dragon
```

Then in Neovim, install the Tree-sitter parser:

```
:TSInstall dragon
```

## How It Works

**Neovim with Tree-sitter** (recommended): The plugin registers a Tree-sitter parser for Dragon and provides `.scm` query files for precise, semantic highlighting.

**Vim / Neovim without Tree-sitter**: Falls back to a traditional `syntax/dragon.vim` file with regex-based highlighting covering all Dragon keywords, types, strings, and constructs.

## What's Highlighted

- Keywords: `if`, `elif`, `else`, `while`, `for`, `match`, `case`, `try`, `catch`, `finally`, `with`, `return`, `yield`, `raise`, `pass`, `break`, `continue`
- Definitions: `def`, `class`, `lambda`, `self()` constructors
- Concurrency: `fire`, `thread`, `async`, `await`
- Storage: `const`, `static`, `extern`
- Built-in functions: `print`, `len`, `range`, `map`, `filter`, `zip`, `sum`, `super`, `Lock`, etc.
- Built-in types: `int`, `float`, `str`, `bool`, `bytes`, `list`, `dict`, `tuple`, `set`, `void`, `ptr`
- Stdlib types: `Optional`, `Union`, `Callable`, `Thread`, `SyncList`, `SyncDict`, etc.
- Exception hierarchy: `Exception`, `ValueError`, `TypeError`, `OSError`, etc.
- Strings: regular, f-strings with `{interpolation}`, raw (`r"..."`), bytes (`b"..."`)
- Type annotations and return types (`-> Type`)
- Decorators (`@staticmethod`, `@classmethod`)
- `extern "C"` FFI blocks
- Comments (`#`)
- Numbers (int, float, hex, octal, binary)

## File Structure

```
vim-dragon/
├── ftdetect/dragon.vim      # .dr filetype detection
├── ftplugin/dragon.vim      # Editor settings (comments, indent)
├── plugin/dragon.lua        # Tree-sitter parser registration (Neovim)
├── syntax/dragon.vim        # Regex syntax highlighting (Vim fallback)
└── queries/dragon/
    ├── highlights.scm       # Tree-sitter highlighting queries
    └── indents.scm          # Tree-sitter indentation queries
```

## Requirements

- **Vim 8+** or **Neovim 0.9+**
- For Tree-sitter highlighting: [nvim-treesitter](https://github.com/nvim-treesitter/nvim-treesitter) and a C compiler

## License

MIT
