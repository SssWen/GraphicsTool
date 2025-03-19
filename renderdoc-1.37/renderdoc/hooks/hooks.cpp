/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2024 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "hooks.h"
#include "common/common.h"

static rdcarray<LibraryHook *> &LibList()
{
  static rdcarray<LibraryHook *> libs;
  return libs;
}

LibraryHook::LibraryHook()
{
  RDCLOG("WEN: LibraryHook Init ---------");
  LibList().push_back(this);//构造函数，每个 LibraryHook 子类的实例在构造时，自动将自身添加到全局列表 LibList() 中
}

void LibraryHooks::RegisterHooks()
{
  RDCLOG("WEN: RegisterHooks ---------Begin");   
  BeginHookRegistration();

  for(LibraryHook *lib : LibList())
    lib->RegisterHooks();
  RDCLOG("WEN: RegisterHooks ---------End");
  EndHookRegistration();
}

void LibraryHooks::RemoveHookCallbacks()
{
  for(LibraryHook *lib : LibList())
    lib->RemoveHooks();
}

void LibraryHooks::OptionsUpdated()
{
  for(LibraryHook *lib : LibList())
    lib->OptionsUpdated();
}
