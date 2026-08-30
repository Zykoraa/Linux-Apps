/*
 * Minimal test payload for Glory Injector (Linux).
 *
 * Build:
 *     gcc -shared -fPIC -o payload_example.so payload_example.c
 *
 * When injected, its constructor runs inside the target process and appends
 * a line to /tmp/glory_injected.log, so you can confirm the injection landed:
 *     tail -f /tmp/glory_injected.log
 */
#include <stdio.h>
#include <unistd.h>
#include <time.h>

__attribute__((constructor))
static void glory_on_load(void)
{
    FILE *f = fopen("/tmp/glory_injected.log", "a");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "injected into pid %d at %s", (int)getpid(), ctime(&now));
        fclose(f);
    }
}
