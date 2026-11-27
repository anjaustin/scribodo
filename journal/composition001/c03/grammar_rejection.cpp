// Candidate-only parser regression. No model, server, socket, or author data.
#include "llama-grammar.h"
#include <cstdio>

int main()
{
    struct Case { const char *text; bool accepted; };
    const Case cases[] = {
        {"root ::= \"a\"{0,0}\n", true},
        {"root ::= \"a\"{1,2}\n", true},
        {"root ::= \"a\"{2,2}\n", true},
        {"root ::= \"a\"{2,1}\n", false},
        {"root ::= \"a\"{5,2}\n", false},
    };
    unsigned failures = 0;
    for (const auto &test : cases) {
        llama_grammar_parser parser;
        const bool actual = parser.parse(test.text);
        if (actual != test.accepted) {
            failures++;
            std::fprintf(stderr, "FAIL expected acceptance=%d: %s", test.accepted, test.text);
        }
    }
    std::printf("%zu checks, %u failures\n", sizeof(cases) / sizeof(cases[0]), failures);
    return failures ? 1 : 0;
}
