#include "reducedmotion.h"

#import <AppKit/NSAccessibility.h>
#import <AppKit/NSWorkspace.h>

bool prefersReducedMotion()
{
    return [NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceMotion;
}
