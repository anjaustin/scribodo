/* macOS's Reduce motion setting, for ui/motion.h. Kept apart from motion.c so the headless tests link without
 * CoreFoundation. */
#include "ui/motion.h"

#include <CoreFoundation/CoreFoundation.h>

bool motion_system_reduced(void)
{
    Boolean valid = false;
    Boolean on = CFPreferencesGetAppBooleanValue(CFSTR("reduceMotion"), CFSTR("com.apple.universalaccess"), &valid);
    return valid && on;
}
