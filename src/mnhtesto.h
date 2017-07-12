#ifndef MNHTESTO_H
#define MNHTESTO_H

#include <mnfcgi_app.h>

#ifdef __cplusplus
extern "C" {
#endif

int mnhtesto_stdin_end(mnfcgi_request_t *, void *);
int mnhtesto_app_init(mnfcgi_app_t *);

#ifdef __cplusplus
}
#endif

#endif /* MNHTESTO_H*/
