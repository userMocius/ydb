# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

OWNER(g:python-contrib)

VERSION(3.0.26)

LICENSE(BSD-3-Clause)

PEERDIR(
    contrib/python/wcwidth
)

NO_LINT()

NO_CHECK_IMPORTS(
    prompt_toolkit.clipboard.pyperclip
    prompt_toolkit.contrib.ssh.*
    prompt_toolkit.contrib.telnet.*
    prompt_toolkit.eventloop.win32
    prompt_toolkit.input.posix_pipe
    prompt_toolkit.input.vt100
    prompt_toolkit.input.win32
    prompt_toolkit.input.win32_pipe
    prompt_toolkit.output.conemu
    prompt_toolkit.output.win32
    prompt_toolkit.output.windows10
    prompt_toolkit.terminal.conemu_output
    prompt_toolkit.win32_types
)

PY_SRCS(
    TOP_LEVEL
    prompt_toolkit/__init__.py
    prompt_toolkit/application/__init__.py
    prompt_toolkit/application/application.py
    prompt_toolkit/application/current.py
    prompt_toolkit/application/dummy.py
    prompt_toolkit/application/run_in_terminal.py
    prompt_toolkit/auto_suggest.py
    prompt_toolkit/buffer.py
    prompt_toolkit/cache.py
    prompt_toolkit/clipboard/__init__.py
    prompt_toolkit/clipboard/base.py
    prompt_toolkit/clipboard/in_memory.py
    prompt_toolkit/clipboard/pyperclip.py
    prompt_toolkit/completion/__init__.py
    prompt_toolkit/completion/base.py
    prompt_toolkit/completion/deduplicate.py
    prompt_toolkit/completion/filesystem.py
    prompt_toolkit/completion/fuzzy_completer.py
    prompt_toolkit/completion/nested.py
    prompt_toolkit/completion/word_completer.py
    prompt_toolkit/contrib/__init__.py
    prompt_toolkit/contrib/completers/__init__.py
    prompt_toolkit/contrib/completers/system.py
    prompt_toolkit/contrib/regular_languages/__init__.py
    prompt_toolkit/contrib/regular_languages/compiler.py
    prompt_toolkit/contrib/regular_languages/completion.py
    prompt_toolkit/contrib/regular_languages/lexer.py
    prompt_toolkit/contrib/regular_languages/regex_parser.py
    prompt_toolkit/contrib/regular_languages/validation.py
    prompt_toolkit/contrib/ssh/__init__.py
    prompt_toolkit/contrib/ssh/server.py
    prompt_toolkit/contrib/telnet/__init__.py
    prompt_toolkit/contrib/telnet/log.py
    prompt_toolkit/contrib/telnet/protocol.py
    prompt_toolkit/contrib/telnet/server.py
    prompt_toolkit/data_structures.py
    prompt_toolkit/document.py
    prompt_toolkit/enums.py
    prompt_toolkit/eventloop/__init__.py
    prompt_toolkit/eventloop/async_context_manager.py
    prompt_toolkit/eventloop/async_generator.py
    prompt_toolkit/eventloop/dummy_contextvars.py
    prompt_toolkit/eventloop/inputhook.py
    prompt_toolkit/eventloop/utils.py
    prompt_toolkit/eventloop/win32.py
    prompt_toolkit/filters/__init__.py
    prompt_toolkit/filters/app.py
    prompt_toolkit/filters/base.py
    prompt_toolkit/filters/cli.py
    prompt_toolkit/filters/utils.py
    prompt_toolkit/formatted_text/__init__.py
    prompt_toolkit/formatted_text/ansi.py
    prompt_toolkit/formatted_text/base.py
    prompt_toolkit/formatted_text/html.py
    prompt_toolkit/formatted_text/pygments.py
    prompt_toolkit/formatted_text/utils.py
    prompt_toolkit/history.py
    prompt_toolkit/input/__init__.py
    prompt_toolkit/input/ansi_escape_sequences.py
    prompt_toolkit/input/base.py
    prompt_toolkit/input/defaults.py
    prompt_toolkit/input/posix_pipe.py
    prompt_toolkit/input/posix_utils.py
    prompt_toolkit/input/typeahead.py
    prompt_toolkit/input/vt100.py
    prompt_toolkit/input/vt100_parser.py
    prompt_toolkit/input/win32.py
    prompt_toolkit/input/win32_pipe.py
    prompt_toolkit/key_binding/__init__.py
    prompt_toolkit/key_binding/bindings/__init__.py
    prompt_toolkit/key_binding/bindings/auto_suggest.py
    prompt_toolkit/key_binding/bindings/basic.py
    prompt_toolkit/key_binding/bindings/completion.py
    prompt_toolkit/key_binding/bindings/cpr.py
    prompt_toolkit/key_binding/bindings/emacs.py
    prompt_toolkit/key_binding/bindings/focus.py
    prompt_toolkit/key_binding/bindings/mouse.py
    prompt_toolkit/key_binding/bindings/named_commands.py
    prompt_toolkit/key_binding/bindings/open_in_editor.py
    prompt_toolkit/key_binding/bindings/page_navigation.py
    prompt_toolkit/key_binding/bindings/scroll.py
    prompt_toolkit/key_binding/bindings/search.py
    prompt_toolkit/key_binding/bindings/vi.py
    prompt_toolkit/key_binding/defaults.py
    prompt_toolkit/key_binding/digraphs.py
    prompt_toolkit/key_binding/emacs_state.py
    prompt_toolkit/key_binding/key_bindings.py
    prompt_toolkit/key_binding/key_processor.py
    prompt_toolkit/key_binding/vi_state.py
    prompt_toolkit/keys.py
    prompt_toolkit/layout/__init__.py
    prompt_toolkit/layout/containers.py
    prompt_toolkit/layout/controls.py
    prompt_toolkit/layout/dimension.py
    prompt_toolkit/layout/dummy.py
    prompt_toolkit/layout/layout.py
    prompt_toolkit/layout/margins.py
    prompt_toolkit/layout/menus.py
    prompt_toolkit/layout/mouse_handlers.py
    prompt_toolkit/layout/processors.py
    prompt_toolkit/layout/screen.py
    prompt_toolkit/layout/scrollable_pane.py
    prompt_toolkit/layout/utils.py
    prompt_toolkit/lexers/__init__.py
    prompt_toolkit/lexers/base.py
    prompt_toolkit/lexers/pygments.py
    prompt_toolkit/log.py
    prompt_toolkit/mouse_events.py
    prompt_toolkit/output/__init__.py
    prompt_toolkit/output/base.py
    prompt_toolkit/output/color_depth.py
    prompt_toolkit/output/conemu.py
    prompt_toolkit/output/defaults.py
    prompt_toolkit/output/vt100.py
    prompt_toolkit/output/win32.py
    prompt_toolkit/output/windows10.py
    prompt_toolkit/patch_stdout.py
    prompt_toolkit/renderer.py
    prompt_toolkit/search.py
    prompt_toolkit/selection.py
    prompt_toolkit/shortcuts/__init__.py
    prompt_toolkit/shortcuts/dialogs.py
    prompt_toolkit/shortcuts/progress_bar/__init__.py
    prompt_toolkit/shortcuts/progress_bar/base.py
    prompt_toolkit/shortcuts/progress_bar/formatters.py
    prompt_toolkit/shortcuts/prompt.py
    prompt_toolkit/shortcuts/utils.py
    prompt_toolkit/styles/__init__.py
    prompt_toolkit/styles/base.py
    prompt_toolkit/styles/defaults.py
    prompt_toolkit/styles/named_colors.py
    prompt_toolkit/styles/pygments.py
    prompt_toolkit/styles/style.py
    prompt_toolkit/styles/style_transformation.py
    prompt_toolkit/token.py
    prompt_toolkit/utils.py
    prompt_toolkit/validation.py
    prompt_toolkit/widgets/__init__.py
    prompt_toolkit/widgets/base.py
    prompt_toolkit/widgets/dialogs.py
    prompt_toolkit/widgets/menus.py
    prompt_toolkit/widgets/toolbars.py
    prompt_toolkit/win32_types.py
)

RESOURCE_FILES(
    PREFIX contrib/python/prompt-toolkit/py3/
    .dist-info/METADATA
    .dist-info/top_level.txt
    prompt_toolkit/py.typed
)

END()

RECURSE_FOR_TESTS(
    tests
)
