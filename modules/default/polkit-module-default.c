/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-module-default.c : PolicyKit module for default policy
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307	 USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stddef.h>
#include <polkit/polkit.h>

/* The symbol that polkit looks up when loading this module */
polkit_bool_t polkit_module_set_functions (PolKitModuleInterface *module_interface);

static polkit_bool_t
_module_init (PolKitModuleInterface *module_interface, 
              int argc, 
              char *argv[])
{
        return TRUE;
}

static void
_module_shutdown (PolKitModuleInterface *module_interface)
{
}

static PolKitResult
_module_can_session_do_action (PolKitModuleInterface *module_interface,
                               PolKitContext         *pk_context,
                               PolKitAction          *action,
                               PolKitSession         *session)
{
        PolKitResult result;
        PolKitPolicyCache *cache;
        PolKitPolicyFileEntry *pfe;

        result = POLKIT_RESULT_NO;
        cache = polkit_context_get_policy_cache (pk_context);
        pfe = polkit_policy_cache_get_entry (cache, action);
        return polkit_policy_default_can_session_do_action (polkit_policy_file_entry_get_default (pfe), 
                                                            action, 
                                                            session);
}

static PolKitResult
_module_can_caller_do_action (PolKitModuleInterface *module_interface,
                              PolKitContext         *pk_context,
                              PolKitAction          *action,
                              PolKitCaller          *caller)
{
        PolKitResult result;
        PolKitPolicyCache *cache;
        PolKitPolicyFileEntry *pfe;

        result = POLKIT_RESULT_NO;
        cache = polkit_context_get_policy_cache (pk_context);
        pfe = polkit_policy_cache_get_entry (cache, action);
        return polkit_policy_default_can_caller_do_action (polkit_policy_file_entry_get_default (pfe), 
                                                           action, 
                                                           caller);
}

polkit_bool_t
polkit_module_set_functions (PolKitModuleInterface *module_interface)
{
        polkit_bool_t ret;

        ret = FALSE;
        if (module_interface == NULL)
                goto out;

        polkit_module_set_func_initialize (module_interface, _module_init);
        polkit_module_set_func_shutdown (module_interface, _module_shutdown);
        polkit_module_set_func_can_session_do_action (module_interface, _module_can_session_do_action);
        polkit_module_set_func_can_caller_do_action (module_interface, _module_can_caller_do_action);

        ret = TRUE;
out:
        return ret;
}