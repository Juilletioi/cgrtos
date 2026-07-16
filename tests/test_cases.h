#ifndef TEST_CASES_H
#define TEST_CASES_H

typedef void (*test_case_fn_t)(void);

typedef struct {
    const char *name;   /* short command name, e.g. "mem" */
    const char *help;   /* one-line description */
    test_case_fn_t run;
} test_case_t;

void test_cases_reset(void);           /* clear pass/fail counters */
int  test_cases_pass_count(void);
int  test_cases_fail_count(void);
const test_case_t *test_cases_get(int *count); /* table, not including synthetic "all" */
/* Run one case by name, or "all" to run every case. Returns 0 if ok found+ran, -1 unknown. */
int  test_cases_run(const char *name);
void test_cases_list(void);            /* printf list of cases */

#endif
