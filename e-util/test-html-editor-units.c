/*
 * Copyright (C) 2016 Red Hat, Inc. (www.redhat.com)
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <locale.h>
#include <e-util/e-util.h>

#include "e-html-editor-private.h"
#include "test-html-editor-units-utils.h"

#define HTML_PREFIX "<html><head></head><body><p data-evo-paragraph=\"\">"
#define HTML_PREFIX_PLAIN "<html><head></head><body style=\"font-family: Monospace;\">" \
	"<p data-evo-paragraph=\"\" style=\"width: 71ch; word-wrap: break-word; word-break: break-word; \">"
#define HTML_SUFFIX "</p></body></html>"

/* The tests do not use the 'user_data' argument, thus the functions avoid them and the typecast is needed. */
typedef void (* ETestFixtureFunc) (TestFixture *fixture, gconstpointer user_data);

static void
test_create_editor (TestFixture *fixture)
{
	g_assert (fixture->editor != NULL);
	g_assert_cmpstr (e_html_editor_get_content_editor_name (fixture->editor), ==, DEFAULT_CONTENT_EDITOR_NAME);

	/* test of the test function */
	g_assert (test_utils_html_equal (fixture, "<span>a</span>", "<sPaN>a</spaN>"));
	g_assert (!test_utils_html_equal (fixture, "<span>A</span>", "<sPaN>a</spaN>"));
}

static void
test_style_bold_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some bold text\n"
		"seq:hCrcrCSrsc\n"
		"action:bold\n",
		HTML_PREFIX "some <b>bold</b> text" HTML_SUFFIX,
		"some bold text"))
		g_test_fail ();
}

static void
test_style_bold_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some \n"
		"action:bold\n"
		"type:bold\n"
		"action:bold\n"
		"type: text\n",
		HTML_PREFIX "some <b>bold</b> text" HTML_SUFFIX,
		"some bold text"))
		g_test_fail ();
}

static void
test_style_italic_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some italic text\n"
		"seq:hCrcrCSrsc\n"
		"action:italic\n",
		HTML_PREFIX "some <i>italic</i> text" HTML_SUFFIX,
		"some italic text"))
		g_test_fail ();
}

static void
test_style_italic_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some \n"
		"action:italic\n"
		"type:italic\n"
		"action:italic\n"
		"type: text\n",
		HTML_PREFIX "some <i>italic</i> text" HTML_SUFFIX,
		"some italic text"))
		g_test_fail ();
}

static void
test_style_underline_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some underline text\n"
		"seq:hCrcrCSrsc\n"
		"action:underline\n",
		HTML_PREFIX "some <u>underline</u> text" HTML_SUFFIX,
		"some underline text"))
		g_test_fail ();
}

static void
test_style_underline_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some \n"
		"action:underline\n"
		"type:underline\n"
		"action:underline\n"
		"type: text\n",
		HTML_PREFIX "some <u>underline</u> text" HTML_SUFFIX,
		"some underline text"))
		g_test_fail ();
}

static void
test_style_monospace_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some monospace text\n"
		"seq:hCrcrCSrsc\n"
		"action:monospaced\n",
		HTML_PREFIX "some <font face=\"monospace\" size=\"3\">monospace</font> text" HTML_SUFFIX,
		"some monospace text"))
		g_test_fail ();
}

static void
test_style_monospace_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some \n"
		"action:monospaced\n"
		"type:monospace\n"
		"action:monospaced\n"
		"type: text\n",
		HTML_PREFIX "some <font face=\"monospace\" size=\"3\">monospace</font> text" HTML_SUFFIX,
		"some monospace text"))
		g_test_fail ();
}

static void
test_undo_text_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some te\n"
		"undo:save\n"	/* 1 */
		"type:tz\n"
		"undo:save\n"	/* 2 */
		"undo:undo\n"
		"undo:undo\n"
		"undo:test:2\n"
		"undo:redo\n"
		"undo:redo\n"
		"undo:test\n"
		"undo:undo:2\n"
		"undo:drop\n"
		"type:xt\n",
		HTML_PREFIX "some text" HTML_SUFFIX,
		"some text"))
		g_test_fail ();
}

static void
test_justify_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"seq:n\n" /* new line, to be able to use HTML_PREFIX macro */
		"type:center\\n\n"
		"type:right\\n\n"
		"type:left\\n\n"
		"seq:uuu\n"
		"action:justify-center\n"
		"seq:d\n"
		"action:justify-right\n"
		"seq:d\n"
		"action:justify-left\n",
		HTML_PREFIX "<br></p>"
			"<p data-evo-paragraph=\"\" style=\"text-align: center\">center</p>"
			"<p data-evo-paragraph=\"\" style=\"text-align: right\">right</p>"
			"<p data-evo-paragraph=\"\">left</p><p data-evo-paragraph=\"\"><br>"
		HTML_SUFFIX,
		"\ncenter\nright\nleft\n"))
		g_test_fail ();
}

static void
test_justify_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"seq:n\n" /* new line, to be able to use HTML_PREFIX macro */
		"action:justify-center\n"
		"type:center\\n\n"
		"action:justify-right\n"
		"type:right\\n\n"
		"action:justify-left\n"
		"type:left\\n\n",
		HTML_PREFIX "<br></p>"
			"<p data-evo-paragraph=\"\" style=\"text-align: center\">center</p>"
			"<p data-evo-paragraph=\"\" style=\"text-align: right\">right</p>"
			"<p data-evo-paragraph=\"\">left</p><p data-evo-paragraph=\"\"><br>"
		HTML_SUFFIX,
		"\ncenter\nright\nleft\n"))
		g_test_fail ();
}

static void
test_indent_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:level 0\\n\n"
		"type:level 1\\n\n"
		"type:level 2\\n\n"
		"type:level 1\\n\n"
		"seq:uuu\n"
		"action:indent\n"
		"seq:d\n"
		"action:indent\n"
		"action:indent\n"
		"seq:d\n"
		"action:indent\n"
		"action:indent\n" /* just to try whether the unindent will work too */
		"action:unindent\n",
		HTML_PREFIX
			"level 0</p>"
			"<div style=\"margin-left: 3ch;\">"
				"<p data-evo-paragraph=\"\">level 1</p>"
				"<div style=\"margin-left: 3ch;\"><p data-evo-paragraph=\"\">level 2</p></div>"
				"<p data-evo-paragraph=\"\">level 1</p>"
			"</div><p data-evo-paragraph=\"\"><br>"
		HTML_SUFFIX,
		"level 0\n"
		"    level 1\n"
		"        level 2\n"
		"    level 1\n"))
		g_test_fail ();
}

static void
test_indent_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:level 0\\n\n"
		"action:indent\n"
		"type:level 1\\n\n"
		"action:indent\n"
		"type:level 2\\n\n"
		"action:unindent\n"
		"type:level 1\\n\n"
		"action:unindent\n",
		HTML_PREFIX
			"level 0</p>"
			"<div style=\"margin-left: 3ch;\">"
				"<p data-evo-paragraph=\"\">level 1</p>"
				"<div style=\"margin-left: 3ch;\"><p data-evo-paragraph=\"\">level 2</p></div>"
				"<p data-evo-paragraph=\"\">level 1</p>"
			"</div><p data-evo-paragraph=\"\"><br>"
		HTML_SUFFIX,
		"level 0\n"
		"    level 1\n"
		"        level 2\n"
		"    level 1\n"))
		g_test_fail ();
}

static void
test_font_size_selection (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some monospace text\n"
		"seq:hCrcrCSrsc\n"
		"action:monospaced\n",
		HTML_PREFIX "some <font face=\"monospace\" size=\"3\">monospace</font> text" HTML_SUFFIX,
		"some monospace text"))
		g_test_fail ();
}

static void
test_font_size_typed (TestFixture *fixture)
{
	if (!test_utils_run_simple_test	(fixture,
		"mode:html\n"
		"type:some \n"
		"action:monospaced\n"
		"type:monospace\n"
		"action:monospaced\n"
		"type: text\n",
		HTML_PREFIX "some <font face=\"monospace\" size=\"3\">monospace</font> text" HTML_SUFFIX,
		"some monospace text"))
		g_test_fail ();
}

gint
main (gint argc,
      gchar *argv[])
{
	gint cmd_delay = -1;
	GOptionEntry entries[] = {
		{ "cmd-delay", '\0', 0,
		  G_OPTION_ARG_INT, &cmd_delay,
		  "Specify delay, in milliseconds, to use during processing commands. Default is 5 ms.",
		  NULL },
		{ NULL }
	};
	GOptionContext *context;
	GError *error = NULL;
	GList *modules;
	gint res;

	setlocale (LC_ALL, "");

	/* Force the memory GSettings backend, to not overwrite user settings
	   when playing with them. It also ensures that the test will run with
	   default settings, until changed. */
	g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

	g_test_init (&argc, &argv, NULL);
	g_test_bug_base ("http://bugzilla.gnome.org/show_bug.cgi?id=");

	gtk_init (&argc, &argv);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_warning ("Failed to parse arguments: %s\n", error ? error->message : "Unknown error");
		g_option_context_free (context);
		g_clear_error (&error);
		return -1;
	}

	g_option_context_free (context);

	if (cmd_delay > 0)
		test_utils_set_event_processing_delay_ms ((guint) cmd_delay);

	e_util_init_main_thread (NULL);
	e_passwords_init ();

	modules = e_module_load_all_in_directory (EVOLUTION_MODULEDIR);
	g_list_free_full (modules, (GDestroyNotify) g_type_module_unuse);

	#define add_test(_name, _func)	\
		g_test_add (_name, TestFixture, NULL, \
			test_utils_fixture_set_up, (ETestFixtureFunc) _func, test_utils_fixture_tear_down)

	add_test ("/create/editor", test_create_editor);
	add_test ("/style/bold/selection", test_style_bold_selection);
	add_test ("/style/bold/typed", test_style_bold_typed);
	add_test ("/style/italic/selection", test_style_italic_selection);
	add_test ("/style/italic/typed", test_style_italic_typed);
	add_test ("/style/underline/selection", test_style_underline_selection);
	add_test ("/style/underline/typed", test_style_underline_typed);
	add_test ("/style/monospace/selection", test_style_monospace_selection);
	add_test ("/style/monospace/typed", test_style_monospace_typed);
	add_test ("/undo/text-typed", test_undo_text_typed);
	add_test ("/justify/selection", test_justify_selection);
	add_test ("/justify/typed", test_justify_typed);
	add_test ("/indent/selection", test_indent_selection);
	add_test ("/indent/typed", test_indent_typed);
	add_test ("/font/size/selection", test_font_size_selection);
	add_test ("/font/size/typed", test_font_size_typed);

	#undef add_test

	res = g_test_run ();

	e_util_cleanup_settings ();
	e_spell_checker_free_global_memory ();

	return res;
}
