/*
 * gz_ach.c - Recomp Achievements (github.com/elripalda/RecompAchievements).
 *
 * The original Xbox game has no achievements, so this recomp sends named
 * events and RecompLauncher's list for the game (hosted by RecompLauncher)
 * turns them into achievements. Events, fired from recomp_manual.c when a
 * match ends ("Match results"):
 *   match_played, match_won                every match / one player 1 won
 *   match_won_<monster>                    e.g. match_won_godzilla2k
 *   match_won_city_<city>                  e.g. match_won_city_new_tokyo
 *   online_match_played, online_match_won  the same over System Link (GZ_NET)
 *
 * Built with the library when setup.ps1 fetched it into
 * third_party\RecompAchievements (GZ_ACHIEVEMENTS). Started without
 * RecompLauncher the library does nothing; without the library, neither does
 * this file. Everything here runs on the game thread (the frame limiter and
 * the script calls that end a match).
 */
#include <stdio.h>

#ifdef GZ_ACHIEVEMENTS
#include <rl_achievements.h>

static int s_state;   /* 0 not started, 1 connected, -1 no launcher */

void gz_ach_frame(void)
{
    if (s_state == 0) {
        s_state = rl_ach_init("xbox", NULL, NULL) ? 1 : -1;
        if (s_state > 0) fprintf(stderr, "[ACH] connected to RecompLauncher\n");
    }
    if (s_state > 0) rl_ach_frame();
}

void gz_ach_event(const char *name)
{
    fprintf(stderr, "[ACH] event %s%s\n", name, s_state > 0 ? "" : " (no launcher)");
    if (s_state > 0) rl_ach_event(name);
}
#else
void gz_ach_frame(void) {}
void gz_ach_event(const char *name) { fprintf(stderr, "[ACH] event %s (built without achievements)\n", name); }
#endif
