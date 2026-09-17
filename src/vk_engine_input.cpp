#include "vk_engine.h"

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "ImGuizmo.h"

bool VulkanEngine::process_event(const SDL_Event& e)
{
    // Close the window when the user presses Alt+F4 or clicks the X.
    if (e.type == SDL_QUIT) {
        return true;
    }

    if(e.type == SDL_WINDOWEVENT) {
        if(e.window.event == SDL_WINDOWEVENT_MINIMIZED){
            stop_rendering = true;
        }
        if(e.window.event == SDL_WINDOWEVENT_RESTORED) {
            stop_rendering = false;
        }
        if(e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
           e.window.event == SDL_WINDOWEVENT_RESIZED) {
            resize_requested = true;
        }
    }
    // Let ImGui observe every event before deciding whether the
    // editor camera should consume it.
    ImGui_ImplSDL2_ProcessEvent(&e);

    if(e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        return true;
    }

    if (e.type == SDL_KEYDOWN &&
        e.key.keysym.sym == SDLK_TAB &&
        e.key.repeat == 0) {
        set_editor_mode(!_editorMode);
    }

    // F9 prints the current view as a MIRABILIS_TEST_CAMERA string.  A bug
    // that only appears from one viewpoint cannot be measured until a
    // headless capture can stand exactly where the eye that found it stood,
    // and reading five numbers back off the screen is how they get there.
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9 &&
        e.key.repeat == 0) {
        const Camera& view = _editorMode ? _editorCamera : mainCamera;
        fmt::print(
            "MIRABILIS_TEST_CAMERA='{:.3f} {:.3f} {:.3f} {:.3f} {:.3f}'\n",
            view.position.x, view.position.y, view.position.z,
            view.pitch, view.yaw);
    }

    // Handle this before the gameplay WASD code below.  Without
    // consuming the event, the S in Ctrl+S also starts backward
    // movement for one frame (or while the key is held).
    if (e.type == SDL_KEYDOWN &&
        e.key.keysym.sym == SDLK_s &&
        (e.key.keysym.mod & KMOD_CTRL) != 0 &&
        e.key.repeat == 0 &&
        !ImGui::GetIO().WantTextInput) {
        save_editor_scene();
        _playerInput.backward = false;
        return false;
    }

    if (!_editorMode &&
        e.type == SDL_KEYDOWN &&
        e.key.keysym.sym == SDLK_r &&
        e.key.repeat == 0) {
        retract_portals();
    }

    if (!_editorMode && e.type == SDL_KEYDOWN && e.key.repeat == 0) {
        if (e.key.keysym.sym == SDLK_F1) {
            respawn_player();
        } else if (e.key.keysym.sym == SDLK_F2) {
            _noClip.enabled = !_noClip.enabled;
            _noClip.up = false;
            _noClip.down = false;
            _playerMovement.velocity = glm::vec3(0.0f);
        }
    }
    // Relative mouse mode hides and locks the cursor, so the Play Controls
    // panel (No Clip Speed among its sliders) is otherwise undraggable while
    // playing. Holding Alt frees the cursor for it without leaving play mode
    // or, unlike Tab, disabling no clip. Movement input is already gated on
    // _mouseCaptured elsewhere, so it naturally pauses while this is held.
    if (!_editorMode && e.type == SDL_KEYDOWN &&
        e.key.keysym.sym == SDLK_LALT && e.key.repeat == 0) {
        set_mouse_capture(false);
    }
    if (!_editorMode && e.type == SDL_KEYUP &&
        e.key.keysym.sym == SDLK_LALT) {
        set_mouse_capture(true);
    }

    if (_editorMode) {
        // Match the conventional ImGuizmo/Unity-style transform
        // bindings, but leave W/A/S/D to the fly camera while
        // RMB is held.
        if (e.type == SDL_KEYDOWN && e.key.repeat == 0 &&
            !_editorCameraLooking &&
            !ImGui::GetIO().WantCaptureKeyboard) {
            if (e.key.keysym.sym == SDLK_w) {
                _editorGizmo.operation = EditorGizmoOperation::Translate;
            } else if (e.key.keysym.sym == SDLK_e) {
                _editorGizmo.operation = EditorGizmoOperation::Rotate;
            } else if (e.key.keysym.sym == SDLK_r) {
                _editorGizmo.operation = EditorGizmoOperation::Scale;
            } else if (e.key.keysym.sym == SDLK_s) {
                _editorGizmo.snapping = !_editorGizmo.snapping;
            }
        }
        if (e.type == SDL_KEYDOWN &&
            e.key.keysym.sym == SDLK_d &&
            (e.key.keysym.mod & KMOD_CTRL) != 0 &&
            e.key.repeat == 0 &&
            !ImGui::GetIO().WantCaptureKeyboard) {
            duplicate_selected_scene_object();
        }
        if (e.type == SDL_KEYDOWN &&
            e.key.keysym.sym == SDLK_DELETE &&
            e.key.repeat == 0 &&
            !ImGui::GetIO().WantCaptureKeyboard) {
            delete_selected_scene_object();
        }

        // The cursor remains free for ImGui. Hold RMB over the
        // scene to temporarily capture it for fly-camera input.
        if (e.type == SDL_MOUSEBUTTONDOWN &&
            e.button.button == SDL_BUTTON_RIGHT &&
            !ImGui::GetIO().WantCaptureMouse) {
            _editorCameraLooking = true;
            set_mouse_capture(true);
        }
        if (e.type == SDL_MOUSEBUTTONUP &&
            e.button.button == SDL_BUTTON_RIGHT &&
            _editorCameraLooking) {
            _editorCameraLooking = false;
            _editorCamera.velocity = glm::vec3(0.0f);
            set_mouse_capture(false);
        }
        if (_editorCameraLooking &&
            (e.type == SDL_MOUSEMOTION ||
             e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)) {
            _editorCamera.processSDLEvent(e);
        }
        if (e.type == SDL_MOUSEBUTTONDOWN &&
            e.button.button == SDL_BUTTON_LEFT &&
            !ImGui::GetIO().WantCaptureMouse &&
            !ImGuizmo::IsOver()) {
            select_scene_object_at_screen_position(
                e.button.x, e.button.y);
        }
    } else {
        if (_mouseCaptured && e.type == SDL_MOUSEMOTION) {
            mainCamera.processSDLEvent(e);
        }

        if (_mouseCaptured && e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_w) _playerInput.forward = true;
            if (e.key.keysym.sym == SDLK_s) _playerInput.backward = true;
            if (e.key.keysym.sym == SDLK_a) _playerInput.left = true;
            if (e.key.keysym.sym == SDLK_d) _playerInput.right = true;
            if (e.key.keysym.sym == SDLK_SPACE) {
                if (_noClip.enabled) _noClip.up = true;
                else if (e.key.repeat == 0) _playerInput.jumpPressed = true;
            }
            if (_noClip.enabled && (e.key.keysym.sym == SDLK_LCTRL ||
                                e.key.keysym.sym == SDLK_RCTRL)) {
                _noClip.down = true;
            }
        }

        if (_mouseCaptured && e.type == SDL_MOUSEWHEEL) {
            int wheelY = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                wheelY = -wheelY;
            }
            if (wheelY < 0) {
                _playerInput.jumpPressed = true;
            }
        }

        if (_mouseCaptured && e.type == SDL_MOUSEBUTTONDOWN &&
            e.button.button == SDL_BUTTON_LEFT) {
            place_portal(_bluePortal, _orangePortal);
        }
        if (_mouseCaptured && e.type == SDL_MOUSEBUTTONDOWN &&
            e.button.button == SDL_BUTTON_RIGHT) {
            place_portal(_orangePortal, _bluePortal);
        }
        if (_mouseCaptured && e.type == SDL_KEYUP) {
            if (e.key.keysym.sym == SDLK_w) _playerInput.forward = false;
            if (e.key.keysym.sym == SDLK_s) _playerInput.backward = false;
            if (e.key.keysym.sym == SDLK_a) _playerInput.left = false;
            if (e.key.keysym.sym == SDLK_d) _playerInput.right = false;
            if (e.key.keysym.sym == SDLK_SPACE) _noClip.up = false;
            if (e.key.keysym.sym == SDLK_LCTRL ||
                e.key.keysym.sym == SDLK_RCTRL) _noClip.down = false;
        }
    }

    return false;
}
