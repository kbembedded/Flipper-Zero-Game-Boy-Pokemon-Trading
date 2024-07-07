#ifndef POKEMON_EXIT_CONFIRM_H
#define POKEMON_EXIT_CONFIRM_H

#pragma once

#include <gui/modules/dialog_ex.h>
#include <gui/scene_manager.h>

void pokemon_exit_confirm_dialog_callback(DialogExResult result, void* context);

void pokemon_exit_confirm_on_enter(void* context);

bool pokemon_exit_confirm_on_event(void* context, SceneManagerEvent event);

#endif // POKEMON_EXIT_CONFIRM_H
