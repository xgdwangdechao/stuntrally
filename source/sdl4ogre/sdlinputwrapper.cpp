#include "pch.h"
#include "sdlinputwrapper.hpp"
#include <SDL_syswm.h>

#include <OgrePlatform.h>
#include <OgreRoot.h>

/*
#if OGRE_PLATFORM == OGRE_PLATFORM_LINUX
#   include <X11/Xlib.h>
#   include <X11/Xutil.h>
#   include <X11/Xos.h>
#endif
*/

namespace SFO
{
    /// \brief General purpose wrapper for OGRE applications around SDL's event
    ///        queue, mostly used for handling input-related events.
    InputWrapper::InputWrapper(SDL_Window* window, Ogre::RenderWindow* ogreWindow) :
        mSDLWindow(window),
        mOgreWindow(ogreWindow),
        mOwnWindow(false),
        mWarpCompensate(false),
        mMouseRelative(false),
        mGrabPointer(false),
        mWrapPointer(false),
        mMouseZ(0),
        mMouseY(0),
        mMouseX(0),
		mMouseInWindow(true),
		mJoyListener(NULL),
		mKeyboardListener(NULL),
		mMouseListener(NULL),
		mWindowListener(NULL)
    {
        _setupOISKeys();
    }

    InputWrapper::~InputWrapper()
    {
        if(mSDLWindow != NULL && mOwnWindow)
            SDL_DestroyWindow(mSDLWindow);
        mSDLWindow = NULL;
    }

    void InputWrapper::capture()
    {
        SDL_Event evt;
        while(SDL_PollEvent(&evt))
        {
            switch(evt.type)
            {
                case SDL_MOUSEMOTION:
                    //ignore this if it happened due to a warp
                    if(!_handleWarpMotion(evt.motion))
                    {
                        mMouseListener->mouseMoved(_packageMouseMotion(evt));

                        //try to keep the mouse inside the window
                        _wrapMousePointer(evt.motion);
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    mMouseListener->mouseMoved(_packageMouseMotion(evt));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    mMouseListener->mousePressed(evt.button, evt.button.button);
                    break;
                case SDL_MOUSEBUTTONUP:
                    mMouseListener->mouseReleased(evt.button, evt.button.button);
                    break;
                case SDL_KEYDOWN:
                    if (!evt.key.repeat)
                        mKeyboardListener->keyPressed(evt.key);
                    break;
                case SDL_KEYUP:
                    if (!evt.key.repeat)
                        mKeyboardListener->keyReleased(evt.key);
                    break;
                case SDL_TEXTINPUT:
                    mKeyboardListener->textInput(evt.text);
                    break;
				case SDL_JOYAXISMOTION:
					if (mJoyListener)
						mJoyListener->axisMoved(evt.jaxis, evt.jaxis.axis);
					break;
				case SDL_JOYBUTTONDOWN:
					if (mJoyListener)
						mJoyListener->buttonPressed(evt.jbutton, evt.jbutton.button);
					break;
				case SDL_JOYBUTTONUP:
					if (mJoyListener)
						mJoyListener->buttonReleased(evt.jbutton, evt.jbutton.button);
					break;
				case SDL_JOYDEVICEADDED:
					//SDL_JoystickOpen(evt.jdevice.which);
					//std::cout << "Detected a new joystick: " << SDL_JoystickNameForIndex(evt.jdevice.which) << std::endl;
					break;
				case SDL_JOYDEVICEREMOVED:
					//std::cout << "A joystick has been removed" << std::endl;
					break;
                case SDL_WINDOWEVENT:
                    handleWindowEvent(evt);
                    break;
                case SDL_QUIT:
                    Ogre::Root::getSingleton().queueEndRendering();
                    break;
                default:
                    std::cerr << "Unhandled SDL event of type " << evt.type << std::endl;
                    break;
            }
        }
    }

    void InputWrapper::handleWindowEvent(const SDL_Event& evt)
    {
        switch (evt.window.event) {
            case SDL_WINDOWEVENT_ENTER:
                mMouseInWindow = true;
                break;
            case SDL_WINDOWEVENT_LEAVE:
                mMouseInWindow = false;
                SDL_SetWindowGrab(mSDLWindow, SDL_FALSE);
                SDL_SetRelativeMouseMode(SDL_FALSE);
                break;
			case SDL_WINDOWEVENT_SIZE_CHANGED:
				int w,h;
				SDL_GetWindowSize(mSDLWindow, &w, &h);
				// TODO: Fix Ogre to handle this more consistently
#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
				mOgreWindow->windowMovedOrResized();
#else
				mOgreWindow->resize(w, h);
#endif
				if (mWindowListener)
					mWindowListener->windowResized(evt.window.data1, evt.window.data2);

			case SDL_WINDOWEVENT_RESIZED:
			// SDL_WINDOWEVENT_SIZE_CHANGED is always sent, so we don't need to care about this one.
				break;

            case SDL_WINDOWEVENT_FOCUS_GAINED:
				if (mWindowListener)
					mWindowListener->windowFocusChange(true);
				break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
				if (mWindowListener)
					mWindowListener->windowFocusChange(false);
				break;
            case SDL_WINDOWEVENT_CLOSE:
                break;
            case SDL_WINDOWEVENT_SHOWN:
                mOgreWindow->setVisible(true);
				if (mWindowListener)
					mWindowListener->windowVisibilityChange(true);
                break;
            case SDL_WINDOWEVENT_HIDDEN:
                mOgreWindow->setVisible(false);
				if (mWindowListener)
					mWindowListener->windowVisibilityChange(false);
                break;
        }
    }

	bool InputWrapper::isModifierHeld(SDL_Keymod mod)
    {
        return SDL_GetModState() & mod;
    }

	bool InputWrapper::isKeyDown(SDL_Scancode key)
	{
		return SDL_GetKeyboardState(NULL)[key];
	}

    /// \brief Moves the mouse to the specified point within the viewport
    void InputWrapper::warpMouse(int x, int y)
    {
        SDL_WarpMouseInWindow(mSDLWindow, x, y);
        mWarpCompensate = true;
        mWarpX = x;
        mWarpY = y;
    }

    /// \brief Locks the pointer to the window
    void InputWrapper::setGrabPointer(bool grab)
    {
        mGrabPointer = grab && mMouseInWindow;
        SDL_SetWindowGrab(mSDLWindow, grab && mMouseInWindow ? SDL_TRUE : SDL_FALSE);
    }

    /// \brief Set the mouse to relative positioning. Doesn't move the cursor
    ///        and disables mouse acceleration.
    void InputWrapper::setMouseRelative(bool relative)
    {
        if(mMouseRelative == relative && mMouseInWindow)
            return;

        mMouseRelative = relative && mMouseInWindow;

        mWrapPointer = false;

        //eep, wrap the pointer manually if the input driver doesn't support
        //relative positioning natively
        int success = SDL_SetRelativeMouseMode(relative && mMouseInWindow ? SDL_TRUE : SDL_FALSE);
        if(relative && mMouseInWindow && success != 0)
            mWrapPointer = true;

        //now remove all mouse events using the old setting from the queue
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_MOUSEMOTION);
    }

    /// \brief Internal method for ignoring relative motions as a side effect
    ///        of warpMouse()
    bool InputWrapper::_handleWarpMotion(const SDL_MouseMotionEvent& evt)
    {
        if(!mWarpCompensate)
            return false;

        //this was a warp event, signal the caller to eat it.
        if(evt.x == mWarpX && evt.y == mWarpY)
        {
            mWarpCompensate = false;
            return true;
        }

        return false;
    }

    /// \brief Wrap the mouse to the viewport
    void InputWrapper::_wrapMousePointer(const SDL_MouseMotionEvent& evt)
    {
        //don't wrap if we don't want relative movements, support relative
        //movements natively, or aren't grabbing anyways
        if(!mMouseRelative || !mWrapPointer || !mGrabPointer)
            return;

        int width = 0;
        int height = 0;

        SDL_GetWindowSize(mSDLWindow, &width, &height);

        const int FUDGE_FACTOR_X = width;
        const int FUDGE_FACTOR_Y = height;

        //warp the mouse if it's about to go outside the window
        if(evt.x - FUDGE_FACTOR_X < 0  || evt.x + FUDGE_FACTOR_X > width
                || evt.y - FUDGE_FACTOR_Y < 0 || evt.y + FUDGE_FACTOR_Y > height)
        {
            warpMouse(width / 2, height / 2);
        }
    }

    /// \brief Package mouse and mousewheel motions into a single event
    MouseMotionEvent InputWrapper::_packageMouseMotion(const SDL_Event &evt)
    {
        MouseMotionEvent pack_evt;
        pack_evt.x = mMouseX;
        pack_evt.xrel = 0;
        pack_evt.y = mMouseY;
        pack_evt.yrel = 0;
        pack_evt.z = mMouseZ;
        pack_evt.zrel = 0;

        if(evt.type == SDL_MOUSEMOTION)
        {
            pack_evt.x = mMouseX = evt.motion.x;
            pack_evt.y = mMouseY = evt.motion.y;
            pack_evt.xrel = evt.motion.xrel;
            pack_evt.yrel = evt.motion.yrel;
        }
        else if(evt.type == SDL_MOUSEWHEEL)
        {
            mMouseZ += pack_evt.zrel = (evt.wheel.y * 120);
            pack_evt.z = mMouseZ;
        }
        else
        {
            throw new std::runtime_error("Tried to package non-motion event!");
        }

        return pack_evt;
    }

    OIS::KeyCode InputWrapper::sdl2OISKeyCode(SDL_Keycode code)
    {
        OIS::KeyCode kc = OIS::KC_UNASSIGNED;

        KeyMap::const_iterator ois_equiv = mKeyMap.find(code);

        if(ois_equiv != mKeyMap.end())
            kc = ois_equiv->second;

        return kc;
    }

    void InputWrapper::_setupOISKeys()
    {
        //lifted from OIS's SDLKeyboard.cpp

        //TODO: Consider switching to scancodes so we
        //can properly support international keyboards
        //look at SDL_GetKeyFromScancode and SDL_GetKeyName
        mKeyMap.insert( KeyMap::value_type(SDLK_UNKNOWN, OIS::KC_UNASSIGNED));
        mKeyMap.insert( KeyMap::value_type(SDLK_ESCAPE, OIS::KC_ESCAPE) );
        mKeyMap.insert( KeyMap::value_type(SDLK_1, OIS::KC_1) );
        mKeyMap.insert( KeyMap::value_type(SDLK_2, OIS::KC_2) );
        mKeyMap.insert( KeyMap::value_type(SDLK_3, OIS::KC_3) );
        mKeyMap.insert( KeyMap::value_type(SDLK_4, OIS::KC_4) );
        mKeyMap.insert( KeyMap::value_type(SDLK_5, OIS::KC_5) );
        mKeyMap.insert( KeyMap::value_type(SDLK_6, OIS::KC_6) );
        mKeyMap.insert( KeyMap::value_type(SDLK_7, OIS::KC_7) );
        mKeyMap.insert( KeyMap::value_type(SDLK_8, OIS::KC_8) );
        mKeyMap.insert( KeyMap::value_type(SDLK_9, OIS::KC_9) );
        mKeyMap.insert( KeyMap::value_type(SDLK_0, OIS::KC_0) );
        mKeyMap.insert( KeyMap::value_type(SDLK_MINUS, OIS::KC_MINUS) );
        mKeyMap.insert( KeyMap::value_type(SDLK_EQUALS, OIS::KC_EQUALS) );
        mKeyMap.insert( KeyMap::value_type(SDLK_BACKSPACE, OIS::KC_BACK) );
        mKeyMap.insert( KeyMap::value_type(SDLK_TAB, OIS::KC_TAB) );
        mKeyMap.insert( KeyMap::value_type(SDLK_q, OIS::KC_Q) );
        mKeyMap.insert( KeyMap::value_type(SDLK_w, OIS::KC_W) );
        mKeyMap.insert( KeyMap::value_type(SDLK_e, OIS::KC_E) );
        mKeyMap.insert( KeyMap::value_type(SDLK_r, OIS::KC_R) );
        mKeyMap.insert( KeyMap::value_type(SDLK_t, OIS::KC_T) );
        mKeyMap.insert( KeyMap::value_type(SDLK_y, OIS::KC_Y) );
        mKeyMap.insert( KeyMap::value_type(SDLK_u, OIS::KC_U) );
        mKeyMap.insert( KeyMap::value_type(SDLK_i, OIS::KC_I) );
        mKeyMap.insert( KeyMap::value_type(SDLK_o, OIS::KC_O) );
        mKeyMap.insert( KeyMap::value_type(SDLK_p, OIS::KC_P) );
        mKeyMap.insert( KeyMap::value_type(SDLK_RETURN, OIS::KC_RETURN) );
        mKeyMap.insert( KeyMap::value_type(SDLK_LCTRL, OIS::KC_LCONTROL));
        mKeyMap.insert( KeyMap::value_type(SDLK_a, OIS::KC_A) );
        mKeyMap.insert( KeyMap::value_type(SDLK_s, OIS::KC_S) );
        mKeyMap.insert( KeyMap::value_type(SDLK_d, OIS::KC_D) );
        mKeyMap.insert( KeyMap::value_type(SDLK_f, OIS::KC_F) );
        mKeyMap.insert( KeyMap::value_type(SDLK_g, OIS::KC_G) );
        mKeyMap.insert( KeyMap::value_type(SDLK_h, OIS::KC_H) );
        mKeyMap.insert( KeyMap::value_type(SDLK_j, OIS::KC_J) );
        mKeyMap.insert( KeyMap::value_type(SDLK_k, OIS::KC_K) );
        mKeyMap.insert( KeyMap::value_type(SDLK_l, OIS::KC_L) );
        mKeyMap.insert( KeyMap::value_type(SDLK_SEMICOLON, OIS::KC_SEMICOLON) );
        mKeyMap.insert( KeyMap::value_type(SDLK_COLON, OIS::KC_COLON) );
        mKeyMap.insert( KeyMap::value_type(SDLK_QUOTE, OIS::KC_APOSTROPHE) );
        mKeyMap.insert( KeyMap::value_type(SDLK_BACKQUOTE, OIS::KC_GRAVE)  );
        mKeyMap.insert( KeyMap::value_type(SDLK_LSHIFT, OIS::KC_LSHIFT) );
        mKeyMap.insert( KeyMap::value_type(SDLK_BACKSLASH, OIS::KC_BACKSLASH) );
        mKeyMap.insert( KeyMap::value_type(SDLK_SLASH, OIS::KC_SLASH) );
        mKeyMap.insert( KeyMap::value_type(SDLK_z, OIS::KC_Z) );
        mKeyMap.insert( KeyMap::value_type(SDLK_x, OIS::KC_X) );
        mKeyMap.insert( KeyMap::value_type(SDLK_c, OIS::KC_C) );
        mKeyMap.insert( KeyMap::value_type(SDLK_v, OIS::KC_V) );
        mKeyMap.insert( KeyMap::value_type(SDLK_b, OIS::KC_B) );
        mKeyMap.insert( KeyMap::value_type(SDLK_n, OIS::KC_N) );
        mKeyMap.insert( KeyMap::value_type(SDLK_m, OIS::KC_M) );
        mKeyMap.insert( KeyMap::value_type(SDLK_COMMA, OIS::KC_COMMA)  );
        mKeyMap.insert( KeyMap::value_type(SDLK_PERIOD, OIS::KC_PERIOD));
        mKeyMap.insert( KeyMap::value_type(SDLK_RSHIFT, OIS::KC_RSHIFT));
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_MULTIPLY, OIS::KC_MULTIPLY) );
        mKeyMap.insert( KeyMap::value_type(SDLK_LALT, OIS::KC_LMENU) );
        mKeyMap.insert( KeyMap::value_type(SDLK_SPACE, OIS::KC_SPACE));
        mKeyMap.insert( KeyMap::value_type(SDLK_CAPSLOCK, OIS::KC_CAPITAL) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F1, OIS::KC_F1) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F2, OIS::KC_F2) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F3, OIS::KC_F3) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F4, OIS::KC_F4) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F5, OIS::KC_F5) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F6, OIS::KC_F6) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F7, OIS::KC_F7) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F8, OIS::KC_F8) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F9, OIS::KC_F9) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F10, OIS::KC_F10) );
        mKeyMap.insert( KeyMap::value_type(SDLK_NUMLOCKCLEAR, OIS::KC_NUMLOCK) );
        mKeyMap.insert( KeyMap::value_type(SDLK_SCROLLLOCK, OIS::KC_SCROLL));
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_7, OIS::KC_NUMPAD7) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_8, OIS::KC_NUMPAD8) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_9, OIS::KC_NUMPAD9) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_MINUS, OIS::KC_SUBTRACT) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_4, OIS::KC_NUMPAD4) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_5, OIS::KC_NUMPAD5) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_6, OIS::KC_NUMPAD6) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_PLUS, OIS::KC_ADD) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_1, OIS::KC_NUMPAD1) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_2, OIS::KC_NUMPAD2) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_3, OIS::KC_NUMPAD3) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_0, OIS::KC_NUMPAD0) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_PERIOD, OIS::KC_DECIMAL) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F11, OIS::KC_F11) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F12, OIS::KC_F12) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F13, OIS::KC_F13) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F14, OIS::KC_F14) );
        mKeyMap.insert( KeyMap::value_type(SDLK_F15, OIS::KC_F15) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_EQUALS, OIS::KC_NUMPADEQUALS) );
        mKeyMap.insert( KeyMap::value_type(SDLK_KP_DIVIDE, OIS::KC_DIVIDE) );
        mKeyMap.insert( KeyMap::value_type(SDLK_SYSREQ, OIS::KC_SYSRQ) );
        mKeyMap.insert( KeyMap::value_type(SDLK_RALT, OIS::KC_RMENU) );
        mKeyMap.insert( KeyMap::value_type(SDLK_HOME, OIS::KC_HOME) );
        mKeyMap.insert( KeyMap::value_type(SDLK_UP, OIS::KC_UP) );
        mKeyMap.insert( KeyMap::value_type(SDLK_PAGEUP, OIS::KC_PGUP) );
        mKeyMap.insert( KeyMap::value_type(SDLK_LEFT, OIS::KC_LEFT) );
        mKeyMap.insert( KeyMap::value_type(SDLK_RIGHT, OIS::KC_RIGHT) );
        mKeyMap.insert( KeyMap::value_type(SDLK_END, OIS::KC_END) );
        mKeyMap.insert( KeyMap::value_type(SDLK_DOWN, OIS::KC_DOWN) );
        mKeyMap.insert( KeyMap::value_type(SDLK_PAGEDOWN, OIS::KC_PGDOWN) );
        mKeyMap.insert( KeyMap::value_type(SDLK_INSERT, OIS::KC_INSERT) );
        mKeyMap.insert( KeyMap::value_type(SDLK_DELETE, OIS::KC_DELETE) );
    }
}
