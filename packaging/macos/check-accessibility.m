#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static NSString *absolutePath(const char *argument) {
  NSString *path = [NSString stringWithUTF8String:argument];
  if (!path.isAbsolutePath) {
    path = [NSFileManager.defaultManager.currentDirectoryPath
        stringByAppendingPathComponent:path];
  }
  return path.stringByStandardizingPath;
}

static id attribute(AXUIElementRef element, CFStringRef name) {
  AXUIElementSetMessagingTimeout(element, 1.0);
  CFTypeRef value = NULL;
  if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess) {
    return nil;
  }
  return CFBridgingRelease(value);
}

static NSString *textValue(id value) {
  if ([value isKindOfClass:[NSString class]]) {
    return value;
  }
  if ([value isKindOfClass:[NSAttributedString class]]) {
    return [value string];
  }
  return nil;
}

static BOOL containsSample(AXUIElementRef element) {
  if ([textValue(attribute(element, kAXValueAttribute))
          containsString:@"The quick brown fox"]) {
    return YES;
  }
  NSNumber *length = attribute(element, kAXNumberOfCharactersAttribute);
  if (![length isKindOfClass:[NSNumber class]] || [length longLongValue] <= 0) {
    return NO;
  }
  CFRange range        = CFRangeMake(0, MIN([length longLongValue], 4096));
  AXValueRef parameter = AXValueCreate(kAXValueCFRangeType, &range);
  for (NSString *name in @[
         (__bridge NSString *)kAXStringForRangeParameterizedAttribute,
         (__bridge NSString *)kAXAttributedStringForRangeParameterizedAttribute
       ]) {
    CFTypeRef value = NULL;
    AXError error   = AXUIElementCopyParameterizedAttributeValue(
        element, (__bridge CFStringRef)name, parameter, &value);
    if (error == kAXErrorSuccess) {
      NSString *text = textValue(CFBridgingRelease(value));
      if ([text containsString:@"The quick brown fox"]) {
        CFRelease(parameter);
        return YES;
      }
    }
  }
  CFRelease(parameter);
  return NO;
}

int main(int argc, const char **argv) {
  @autoreleasepool {
    if (!AXIsProcessTrusted()) {
      fprintf(stderr,
              "macOS native accessibility readback is blocked by TCC: grant "
              "Accessibility access to this checker executable on the test "
              "runner, then rerun. No permission dialog was opened.\n");
      return 2;
    }
    if (argc == 2 && strcmp(argv[1], "--check-trust") == 0) {
      return 0;
    }
    if (argc != 3) {
      fprintf(stderr,
              "usage: check-accessibility <xuzz executable> <fixture>\n");
      return 1;
    }
    NSString *executable = absolutePath(argv[1]);
    NSString *fixture    = absolutePath(argv[2]);
    NSString *scratch    = [NSTemporaryDirectory()
        stringByAppendingPathComponent:
            [@"gleditor-ax-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSError *error       = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:scratch
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&error]) {
      fprintf(stderr, "Cannot create isolated test directory: %s\n",
              error.localizedDescription.UTF8String);
      return 1;
    }
    NSString *log = [scratch stringByAppendingPathComponent:@"application.log"];
    [NSFileManager.defaultManager createFileAtPath:log
                                          contents:nil
                                        attributes:nil];
    NSFileHandle *output      = [NSFileHandle fileHandleForWritingAtPath:log];
    NSTask *application       = [[NSTask alloc] init];
    application.executableURL = [NSURL fileURLWithPath:executable];
    application.currentDirectoryURL =
        [NSURL fileURLWithPath:executable.stringByDeletingLastPathComponent];
    application.arguments = @[
      @"--backend", @"opengl", @"--no-present", @"--import", fixture,
      [scratch stringByAppendingPathComponent:@"store"]
    ];
    NSMutableDictionary *environment =
        [NSProcessInfo.processInfo.environment mutableCopy];
    environment[@"XDG_DATA_HOME"] =
        [scratch stringByAppendingPathComponent:@"data"];
    environment[@"XDG_CONFIG_HOME"] =
        [scratch stringByAppendingPathComponent:@"config"];
    environment[@"SDL_AUDIODRIVER"] = @"dummy";
    // Native AX requires Cocoa's NSWindow; --no-present keeps it hidden.
    environment[@"SDL_VIDEODRIVER"] = @"cocoa";
    application.environment         = environment;
    application.standardOutput      = output;
    application.standardError       = output;
    if (![application launchAndReturnError:&error]) {
      fprintf(stderr, "Cannot start test application: %s\n",
              error.localizedDescription.UTF8String);
      return 1;
    }
    AXUIElementRef root =
        AXUIElementCreateApplication(application.processIdentifier);
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:60];
    @try {
      while ([deadline timeIntervalSinceNow] > 0 && application.running) {
        NSMutableArray *pending =
            [NSMutableArray arrayWithObject:(__bridge id)root];
        NSMutableSet *seen = [NSMutableSet set];
        while (pending.count && [deadline timeIntervalSinceNow] > 0) {
          id object = pending.lastObject;
          [pending removeLastObject];
          if ([seen containsObject:object]) {
            continue;
          }
          [seen addObject:object];
          AXUIElementRef element = (__bridge AXUIElementRef)object;
          if (containsSample(element)) {
            printf("NSAccessibility read back imported document text through "
                   "AXUIElement (%lu elements inspected).\n",
                   (unsigned long)seen.count);
            return 0;
          }
          for (NSString *name in @[
                 (__bridge NSString *)kAXWindowsAttribute,
                 (__bridge NSString *)kAXChildrenAttribute
               ]) {
            id children = attribute(element, (__bridge CFStringRef)name);
            if (![children isKindOfClass:[NSArray class]]) {
              continue;
            }
            for (id child in children) {
              if (CFGetTypeID((__bridge CFTypeRef)child) ==
                  AXUIElementGetTypeID()) {
                [pending addObject:child];
              }
            }
          }
        }
        usleep(250000);
      }
      fprintf(stderr,
              "NSAccessibility did not expose fixture text within 60 seconds; "
              "application log: %s\n",
              log.UTF8String);
      return 1;
    } @finally {
      CFRelease(root);
      if (application.running) {
        [application terminate];
        NSDate *stopDeadline = [NSDate dateWithTimeIntervalSinceNow:2];
        while (application.running && [stopDeadline timeIntervalSinceNow] > 0) {
          usleep(50000);
        }
        if (application.running) {
          kill(application.processIdentifier, SIGKILL);
        }
        [application waitUntilExit];
      }
      [output closeFile];
    }
  }
}
