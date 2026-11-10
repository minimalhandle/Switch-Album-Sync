#include "nso_album_sync/auth_callback.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <CoreServices/CoreServices.h>

@interface CaptureDockAuthUrlDelegate : NSObject <NSApplicationDelegate>
- (void)handleGetURLEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)replyEvent;
@end

@implementation CaptureDockAuthUrlDelegate
- (void)handleGetURLEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)replyEvent {
    (void)replyEvent;
    NSString* urlString = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    if (urlString == nil) {
        urlString = [event stringValue];
    }
    if (urlString != nil) {
        const char* utf8 = urlString.UTF8String;
        if (utf8 != nullptr) nso::publish_nintendo_auth_callback(utf8);
    }
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    for (NSURL* url in urls) {
        NSString* absolute = url.absoluteString;
        if (absolute == nil) continue;
        const char* utf8 = absolute.UTF8String;
        if (utf8 != nullptr) nso::publish_nintendo_auth_callback(utf8);
    }
}

- (BOOL)application:(NSApplication*)application openURL:(NSURL*)url {
    (void)application;
    if (url != nil && url.absoluteString != nil) {
        const char* utf8 = url.absoluteString.UTF8String;
        if (utf8 != nullptr) return nso::publish_nintendo_auth_callback(utf8);
    }
    return YES;
}
@end

namespace nso {
namespace {

CaptureDockAuthUrlDelegate* g_auth_url_delegate = nil;

void install_auth_delegate() {
    static dispatch_once_t once_token;
    dispatch_once(&once_token, ^{
        g_auth_url_delegate = [CaptureDockAuthUrlDelegate new];
        [[NSAppleEventManager sharedAppleEventManager]
            setEventHandler:g_auth_url_delegate
            andSelector:@selector(handleGetURLEvent:withReplyEvent:)
            forEventClass:kInternetEventClass
            andEventID:kAEGetURL];
        [NSApp setDelegate:g_auth_url_delegate];
    });
}

}  // namespace

bool register_nintendo_auth_protocol() {
    NSBundle* main_bundle = [NSBundle mainBundle];
    NSString* bundle_id = [main_bundle bundleIdentifier];
    NSURL* bundle_url = [main_bundle bundleURL];
    if (bundle_id == nil || bundle_url == nil) return false;

    const CFStringRef scheme = CFSTR("npf71b963c1b7b6d119");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const auto handler = LSCopyDefaultHandlerForURLScheme(scheme);
    if (handler != nullptr) {
        const bool ours = CFEqual(handler, (__bridge CFStringRef)bundle_id);
        CFRelease(handler);
        if (!ours) return false;
        install_auth_delegate();
        return true;
    }
    if (LSRegisterURL((__bridge CFURLRef)bundle_url, true) != noErr ||
        LSSetDefaultHandlerForURLScheme(scheme, (__bridge CFStringRef)bundle_id) != noErr) {
        return false;
    }
#pragma clang diagnostic pop
    install_auth_delegate();
    return true;
}

void unregister_nintendo_auth_protocol() {
}

}  // namespace nso

#endif  // __APPLE__
