#ifndef SMTP_H
#define SMTP_H

#include "types.h"

/* SMTP default ports */
#define SMTP_PORT     25
#define SMTP_SUBMIT   587   /* submission port (with STARTTLS) */

/**
 * smtp_send - Send an email via SMTP
 * @server_ip:  IP address of SMTP server (host byte order)
 * @port:       TCP port (usually 25 or 587)
 * @from:       Envelope sender / From address (e.g. "user@example.com")
 * @to:         Envelope recipient / To address
 * @subject:    Email subject line
 * @body:       Email body text
 *
 * Returns 0 on success, -1 on error.
 */
int smtp_send(uint32_t server_ip, uint16_t port,
              const char *from, const char *to,
              const char *subject, const char *body);

/**
 * smtp_send_auth - Send email with AUTH LOGIN
 * @server_ip:  IP address of SMTP server (host byte order)
 * @port:       TCP port (usually 25 or 587)
 * @from:       Envelope sender
 * @to:         Envelope recipient
 * @subject:    Email subject line
 * @body:       Email body text
 * @username:   SMTP auth username (plain text)
 * @password:   SMTP auth password (plain text)
 *
 * Returns 0 on success, -1 on error.
 */
int smtp_send_auth(uint32_t server_ip, uint16_t port,
                   const char *from, const char *to,
                   const char *subject, const char *body,
                   const char *username, const char *password);

#endif /* SMTP_H */
