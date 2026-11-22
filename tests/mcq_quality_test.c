#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ai.c"

int main(void) {
    char math[128] = "n \\times \\text{sizeof}";
    normalize_ai_text(math, sizeof(math));
    assert(strcmp(math, "n × sizeof") == 0);

    char escaped_newline[128] = "n \\n bytes";
    normalize_ai_text(escaped_newline, sizeof(escaped_newline));
    assert(strcmp(escaped_newline, "n bytes") == 0);

    char c_code[128] = "printf(\"\\\\n\")";
    normalize_ai_text(c_code, sizeof(c_code));
    assert(strcmp(c_code, "printf(\"\\\\n\")") == 0);

    assert(has_unrendered_math_command("Use \\frac{1}{2}"));
    assert(!has_unrendered_math_command("n × sizeof(T)"));
    assert(has_unrendered_math_command("printf(\"x\") + \\frac{1}{2}"));
    assert(!has_unrendered_math_command("printf(\"\\n\")"));

    Question first = {0}, second = {0};
    snprintf(first.question, sizeof(first.question),
             "What happens when integer n is added to a pointer of type int?");
    snprintf(second.question, sizeof(second.question),
             "What happens when an integer is added to a pointer of type int?");
    assert(duplicate_question(&first, &second, 1));

    puts("MCQ quality tests: PASS");
    return 0;
}
