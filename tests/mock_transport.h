/*
 * A capturing transport for tests: instead of a socket, it records each batch
 * body so a test can assert the exact wire shape. Install after ph_init().
 */
#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H

void mock_install(void);        /* replace the SDK transport with this one */
void mock_reset(void);          /* clear captured batches + flags response; status -> 200 */
void mock_set_status(int status); /* what send()/fetch() returns (e.g. 500 to fail) */
void mock_set_flags_response(const char *json); /* body fetch() returns for /flags/ */
int mock_batch_count(void);
const char *mock_batch(int i);  /* body of the i-th captured batch, or NULL */
const char *mock_last_fetch_url(void);

#endif /* MOCK_TRANSPORT_H */
