/*
  
 Copyright (c) 2008 NFG Net Facilities Group BV support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <stdlib.h>
#include <search.h>
#include <assert.h>
#include <glib.h>

#include "dm_sset.h"

#define THIS_MODULE "SSET"

/*
 * implements the Sorted Set interface using GTree
 */

#define T Sset_T

struct T {
	void *root;
	int (*cmp)(const void *, const void *);
	int len;
	size_t size; // sizeof key
};


T Sset_new(int (*cmp)(const void *a, const void *b), size_t size)
{
	T S;
	assert(size > 0);
	S = calloc(1, sizeof(*S));
	S->root = (void *)g_tree_new((GCompareFunc)cmp);
	S->cmp = cmp;
	S->size = size;
	return S;
}

int Sset_has(T S, const void *a)
{
	return g_tree_lookup((GTree *)S->root, a)?1:0;
}


void Sset_add(T S, const void *a)
{
	if (! Sset_has(S, a))
		g_tree_insert((GTree *)S->root, (void *)a, (void *)a);
}

int Sset_len(T S)
{
	return g_tree_nnodes((GTree *)S->root);
}

void * Sset_del(T S, const void * a)
{
	void * t = NULL;
	if ((t = g_tree_lookup((GTree *)S->root, a)) != NULL)
		g_tree_remove((GTree *)S->root, a);
	return t;
}

void Sset_map(T S, int (*func)(void *, void *, void *), void *data)
{
	g_tree_foreach((GTree *)S->root, (GTraverseFunc)func, data);
}

void Sset_free(T *S)
{
	T s = *S;
	if (s) free(s);
	s = NULL;
}

static int sset_copy(void *a, void *b, void *c)
{
	T t = (T)c;
	if (! Sset_has(c, a)) {
		void * item = malloc(t->size);
		memcpy(item, (const void *)a, t->size);
		Sset_add(t, item);
	}	       
	return 0;
}

T Sset_or(T a, T b) // a + b
{
	T c = Sset_new(a->cmp, a->size);

	Sset_map(a, sset_copy, c);
	Sset_map(b, sset_copy, c);

	return c;
}

T Sset_and(T a, T b) // a * b
{
	return a;
}

T Sset_not(T a, T b) // a - b
{
	return a;
}

T Sset_xor(T a, T b) // a / b
{
	return a;
}

