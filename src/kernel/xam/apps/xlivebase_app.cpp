/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/system/xenumerator.h>
#include <rex/thread.h>
#include "xlivebase_request.h"
#include "../live_sessions.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

namespace {

// X_ONLINE_FRIEND record size and the Xbox Live friends-list limit.
constexpr uint32_t kOnlineFriendSize = 0xC4;
constexpr uint32_t kMaxFriends = 100;

}  // namespace

// Friends enumeration with no service: an enumerator that reports no
// friends. Titles that read the list during Live sign-in treat a failed
// create as a broken profile.
X_HRESULT XLiveBaseApp::CreateFriendsEnumerator(uint32_t arguments_ptr) {
  FriendsEnumeratorRequest request{};
  if (!TryDecodeFriendsEnumeratorRequest(*memory_, arguments_ptr, request)) {
    return X_E_INVALIDARG;
  }

  const uint32_t user_index =
      memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(request.user_index_ptr));
  const uint32_t friend_count =
      memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(request.friend_count_ptr));
  REXKRNL_DEBUG("CXLiveFriends::Enumerate({}, {})", user_index, friend_count);
  if (user_index >= 4 || !friend_count || friend_count > kMaxFriends) {
    return X_E_INVALIDARG;
  }
  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  auto e = make_object<XStaticUntypedEnumerator>(kernel_state_, friend_count, kOnlineFriendSize);
  const auto result = e->Initialize(user_index, app_id(), 0x00058020, 0, 0);
  if (XFAILED(result)) {
    return X_E_FAIL;
  }

  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(request.buffer_size_ptr),
                                   friend_count * kOnlineFriendSize);
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(request.handle_ptr), e->handle());
  return X_E_SUCCESS;
}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // The argument list arrives in the second parameter.
      return CreateFriendsEnumerator(buffer_length);
    }
    case 0x00058023: {
      // XInviteGetAcceptedInfo for a pending Live invite; the argument list
      // arrives in the second parameter.
      if (auto sessions = live::ActiveService()) {
        if (live::HandleInviteInfo(*sessions, *memory_, buffer_length, kernel_state_->title_id()))
          return X_E_SUCCESS;
      }
      REXKRNL_DEBUG("XInviteGetAcceptedInfo({:08X}, {:08X}): no accepted invite", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
