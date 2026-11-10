#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * Giao thuc ung dung (text, moi thong diep la 1 dong ket thuc bang "\r\n")
 *
 * UDP (quet thiet bi, cong DEFAULT_SCAN_PORT):
 *   App -> broadcast : SCAN <app_id>
 *   Thiet bi -> App  : DEVICE <device_id> <type> <tcp_port> <ON|OFF>
 *
 * TCP (moi thiet bi lang nghe tren cong rieng):
 *   CONNECT <app_id> <password>      -> 200 <token> <device_id> <type>
 *   CHPASS  <token> <old> <new>      -> 200 ...
 *   POWER   <token> ON|OFF           -> 200 ...
 *   INFO    <token>                  -> 200 key=value key=value ...
 *   SET     <token> key=value ...    -> 200 ...
 *   RESET   <token>                  -> 200 ...   (khoi tao lai thong so mac dinh)
 *   RUN     <token> [tham so]        -> 200 ...   (dieu khien truc tiep)
 *   LOG     <token> [n]              -> 210 <k>, sau do k dong log
 *   QUIT                             -> 200 Bye, server dong ket noi
 *
 * Phan hoi: "<ma> <noi dung>"
 */

#define DEFAULT_SCAN_PORT 9999
#define MAX_LINE 1024
#define TOKEN_LEN 16

#define RC_OK             200
#define RC_LIST           210
#define RC_BAD_REQUEST    400
#define RC_UNAUTHORIZED   401
#define RC_WRONG_PASSWORD 403
#define RC_UNKNOWN_CMD    404
#define RC_DEVICE_OFF     409
#define RC_SERVER_ERROR   500

#define TYPE_SENSOR     "SENSOR"
#define TYPE_PUMP       "PUMP"
#define TYPE_FERTILIZER "FERTILIZER"
#define TYPE_LIGHT      "LIGHT"

#endif
