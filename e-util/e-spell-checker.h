/*
 * e-spell-checker.h
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef E_SPELL_CHECKER_H
#define E_SPELL_CHECKER_H

#include <glib-object.h>
#include <e-util/e-spell-dictionary.h>

/* Standard GObject macros */
#define E_TYPE_SPELL_CHECKER \
	(e_spell_checker_get_type ())
#define E_SPELL_CHECKER(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SPELL_CHECKER, ESpellChecker))
#define E_SPELL_CHECKER_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SPELL_CHECKER, ESpellCheckerClass))
#define E_IS_SPELL_CHECKER(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SPELL_CHECKER))
#define E_IS_SPELL_CHECKER_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_SPELL_CHECKER))
#define E_SPELL_CHECKER_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SPELL_CHECKER, ESpellCheckerClass))


G_BEGIN_DECLS

typedef struct _ESpellChecker ESpellChecker;
typedef struct _ESpellCheckerPrivate ESpellCheckerPrivate;
typedef struct _ESpellCheckerClass ESpellCheckerClass;

struct _ESpellChecker {
	GObject parent;

	ESpellCheckerPrivate *priv;
};

struct _ESpellCheckerClass {
	GObjectClass parent_class;
};

GType			e_spell_checker_get_type	(void);

ESpellChecker *		e_spell_checker_new		(void);

GList *			e_spell_checker_list_available_dicts
							(ESpellChecker *checker);
ESpellDictionary *	e_spell_checker_lookup_dictionary
							(ESpellChecker *checker,
							 const gchar *language_code);

void			e_spell_checker_set_active_dictionaries
							(ESpellChecker *checker,
							 GList *active_dicts);
GList *			e_spell_checker_get_active_dictionaries
							(ESpellChecker *checker);

void			e_spell_checker_free_dict	(ESpellChecker *checker,
							 EnchantDict *dict);

void			e_spell_checker_learn_word	(ESpellChecker *checker,
							 const gchar *word);
void			e_spell_checker_ignore_word	(ESpellChecker *checker,
							 const gchar *word);

G_END_DECLS


#endif /* E_SPELL_CHECKER_H */
