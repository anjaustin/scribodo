#include "observer_arms.h"

/* journal/observer-cycle2_quality.md. Conflicts on a planted fault over conflicts shown, both planted novels (A Princess
 * of Mars and Pride and Prejudice). The default row's text is the one shown since step 4, unchanged. */
static const ObserverArm k_arms[] = {
    { false, OBSERVER_SENTENCES_OFF, "default",
      "Possible conflicts are suggestions: about 1 in 13 was real on the test novels. Each links to its source.",
      "7 of 92 (step 4, end to end)" },
    { true, OBSERVER_SENTENCES_OFF, "conflict gate",
      "Possible conflicts are suggestions: about 1 in 30 was real on the test novels. Each links to its source.",
      "5 of 149: Mars 3 of 64, P&P 2 of 85 (step 5, end to end)" },
    { false, OBSERVER_SENTENCES_ATTRIBUTES, "sentences, attributes",
      "Possible conflicts are suggestions: about 1 in 5 was real on the test novels. Each links to its source.",
      "11 of 60: Mars 6 of 42, P&P 5 of 18 (step 8, end to end)" },
    { false, OBSERVER_SENTENCES_SUBJECT, "sentences, subject",
      "Possible conflicts are suggestions: about 3 in 4 were real on the test novels, scored on recorded replies. Each "
      "links to its source.",
      "9 of 12: Mars 4 of 5, P&P 5 of 7 (step 9, scored offline on step 8's recorded judge replies)" },
    { true, OBSERVER_SENTENCES_ATTRIBUTES, "conflict gate, sentences, attributes",
      "Possible conflicts are suggestions; this setting was not measured on the test novels. Each links to its source.",
      "not measured" },
    { true, OBSERVER_SENTENCES_SUBJECT, "conflict gate, sentences, subject",
      "Possible conflicts are suggestions; this setting was not measured on the test novels. Each links to its source.",
      "not measured" },
};

const ObserverArm *observer_arms(size_t *n)
{
    *n = sizeof(k_arms) / sizeof(k_arms[0]);
    return k_arms;
}

const ObserverArm *observer_arm(const Experiments *e)
{
    for (size_t i = 0; i < sizeof(k_arms) / sizeof(k_arms[0]); i++)
        if (k_arms[i].conflict_gate == e->observer_conflict_gate && k_arms[i].sentences == e->observer_sentences) return &k_arms[i];
    return &k_arms[0]; /* unreachable while every combination has a row (the test checks it) */
}
