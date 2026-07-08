#include <stdio.h>

int g_checks = 0;
int g_failures = 0;

void suite_json(void);
void suite_props(void);
void suite_queue(void);
void suite_serialize(void);
void suite_capture(void);
void suite_scrub(void);
void suite_exception(void);
void suite_offline(void);
void suite_jsonparse(void);
void suite_flags(void);
void suite_gzip(void);
void suite_http(void);
void suite_ratelimit(void);
void suite_crash(void);

int main(void) {
    printf("posthog-c test suite\n");

    printf("[json]\n");      suite_json();
    printf("[props]\n");     suite_props();
    printf("[queue]\n");     suite_queue();
    printf("[serialize]\n"); suite_serialize();
    printf("[capture]\n");   suite_capture();
    printf("[scrub]\n");     suite_scrub();
    printf("[exception]\n"); suite_exception();
    printf("[offline]\n");   suite_offline();
    printf("[jsonparse]\n"); suite_jsonparse();
    printf("[flags]\n");     suite_flags();
    printf("[gzip]\n");      suite_gzip();
    printf("[http]\n");      suite_http();
    printf("[ratelimit]\n"); suite_ratelimit();
    printf("[crash]\n");     suite_crash();

    printf("\n%d checks, %d failure%s\n", g_checks, g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
