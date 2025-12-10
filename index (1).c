#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PEER_NAME_LEN      10
#define CONTENT_NAME_LEN   10
#define IP_STRLEN          16
#define TABLE_MAX          512

#define PDU_R  'R'
#define PDU_S  'S'
#define PDU_T  'T'
#define PDU_O  'O'
#define PDU_A  'A'
#define PDU_E  'E'
#define PDU_D  'D'
#define PDU_C  'C'

typedef struct __attribute__((packed)) {
    char type;
    char peer[PEER_NAME_LEN];
    char content[CONTENT_NAME_LEN];
    char ip[IP_STRLEN];
    uint16_t port_net;
} PDU;

typedef struct {
    int in_use;
    char peer[PEER_NAME_LEN+1];
    char content[CONTENT_NAME_LEN+1];
    char ip[IP_STRLEN];
    uint16_t port;
    uint32_t use_count;
} Row;

static Row table_[TABLE_MAX];

static void reset_pdu(PDU *p) { memset(p, 0, sizeof(*p)); }

static void copy_field_padded(char *dst, size_t dsz, const char *src, size_t limit) {
    size_t n = 0;
    for (; n < limit && src[n]; ++n) dst[n] = src[n];
    for (; n < dsz; ++n) dst[n] = '\0';
}

static int lookup_same_entry(const char *peer, const char *content) {
    int i;
    for (i = 0; i < TABLE_MAX; ++i) {
        if (!table_[i].in_use) continue;
        if (strncmp(table_[i].peer, peer, PEER_NAME_LEN) == 0 &&
            strncmp(table_[i].content, content, CONTENT_NAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static int choose_least_used_row(const char *content) {
    int i;
    int sel = -1;
    uint32_t best = 0;

    for (i = 0; i < TABLE_MAX; ++i) {
        if (!table_[i].in_use) continue;
        if (strncmp(table_[i].content, content, CONTENT_NAME_LEN) != 0) continue;
        if (sel < 0 || table_[i].use_count < best) {
            sel  = i;
            best = table_[i].use_count;
        }
    }
    return sel;
}

static void process_register(int sock, const struct sockaddr_in *cli, socklen_t clen, const PDU *req) {
    PDU resp; reset_pdu(&resp);

    char peer[PEER_NAME_LEN+1];   memset(peer, 0, sizeof(peer));   copy_field_padded(peer,  sizeof(peer),  req->peer,    PEER_NAME_LEN);
    char cont[CONTENT_NAME_LEN+1];memset(cont, 0, sizeof(cont));   copy_field_padded(cont,  sizeof(cont),  req->content, CONTENT_NAME_LEN);
    char ip[IP_STRLEN];           memset(ip,   0, sizeof(ip));     copy_field_padded(ip,    sizeof(ip),    req->ip,      IP_STRLEN-1);
    uint16_t port = ntohs(req->port_net);

    if (peer[0] == '\0' || cont[0] == '\0' || ip[0] == '\0' || port == 0) {
        resp.type = PDU_E;
        sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
        return;
    }

    if (lookup_same_entry(peer, cont) >= 0) {
        resp.type = PDU_E;
        sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
        return;
    }

    int i;
    for (i = 0; i < TABLE_MAX; ++i) {
        if (!table_[i].in_use) break;
    }
    if (i == TABLE_MAX) {
        resp.type = PDU_E;
        sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
        return;
    }

    table_[i].in_use = 1;
    copy_field_padded(table_[i].peer,    sizeof(table_[i].peer),    peer, PEER_NAME_LEN);
    copy_field_padded(table_[i].content, sizeof(table_[i].content), cont, CONTENT_NAME_LEN);
    copy_field_padded(table_[i].ip,      sizeof(table_[i].ip),      ip,   IP_STRLEN-1);
    table_[i].port = port;
    table_[i].use_count = 0;

    resp.type = PDU_A;
    sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
}

static void process_search(int sock, const struct sockaddr_in *cli, socklen_t clen, const PDU *req) {
    PDU resp; reset_pdu(&resp);

    char cont[CONTENT_NAME_LEN+1]; memset(cont, 0, sizeof(cont)); copy_field_padded(cont, sizeof(cont), req->content, CONTENT_NAME_LEN);

    if (cont[0] == '\0') {
        resp.type = PDU_E;
        sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
        return;
    }

    int sel = choose_least_used_row(cont);
    if (sel < 0) {
        resp.type = PDU_E;
        sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
        return;
    }

    resp.type = PDU_S;
    copy_field_padded(resp.peer,    sizeof(resp.peer),    table_[sel].peer,    PEER_NAME_LEN);
    copy_field_padded(resp.content, sizeof(resp.content), table_[sel].content, CONTENT_NAME_LEN);
    copy_field_padded(resp.ip,      sizeof(resp.ip),      table_[sel].ip,      IP_STRLEN-1);
    resp.port_net = htons(table_[sel].port);

    sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
    table_[sel].use_count += 1;
}

static void process_deregister(int sock, const struct sockaddr_in *cli, socklen_t clen, const PDU *req) {
    PDU resp; reset_pdu(&resp);

    char peer[PEER_NAME_LEN+1];   memset(peer, 0, sizeof(peer));   copy_field_padded(peer,  sizeof(peer),  req->peer,    PEER_NAME_LEN);
    char cont[CONTENT_NAME_LEN+1];memset(cont, 0, sizeof(cont));   copy_field_padded(cont,  sizeof(cont),  req->content, CONTENT_NAME_LEN);

    int i;
    for (i = 0; i < TABLE_MAX; ++i) {
        if (!table_[i].in_use) continue;
        if (strncmp(table_[i].peer, peer, PEER_NAME_LEN) == 0 &&
            strncmp(table_[i].content, cont, CONTENT_NAME_LEN) == 0) {
            table_[i].in_use = 0;
            resp.type = PDU_A;
            sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
            return;
        }
    }

    resp.type = PDU_E;
    sendto(sock, &resp, sizeof(resp), 0, (const struct sockaddr*)cli, clen);
}

static void process_list(int sock, const struct sockaddr_in *cli, socklen_t clen) {
    int i;
    PDU row;

    for (i = 0; i < TABLE_MAX; ++i) {
        if (!table_[i].in_use) continue;

        reset_pdu(&row);
        row.type = PDU_O;
        copy_field_padded(row.peer,    sizeof(row.peer),    table_[i].peer,    PEER_NAME_LEN);
        copy_field_padded(row.content, sizeof(row.content), table_[i].content, CONTENT_NAME_LEN);
        copy_field_padded(row.ip,      sizeof(row.ip),      table_[i].ip,      IP_STRLEN-1);
        row.port_net = htons(table_[i].port);

        sendto(sock, &row, sizeof(row), 0, (const struct sockaddr*)cli, clen);
    }

    PDU end; reset_pdu(&end); end.type = PDU_O;
    sendto(sock, &end, sizeof(end), 0, (const struct sockaddr*)cli, clen);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <udp_port>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Port number must be in range 1..65535\n");
        return 1;
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return 1;
    }

    printf("P2P index now waiting on UDP port %d\n", port);
    memset(table_, 0, sizeof(table_));

    for (;;) {
        PDU req;
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        ssize_t n;

        n = recvfrom(s, &req, sizeof(req), 0, (struct sockaddr*)&cli, &clen);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        if (n != (ssize_t)sizeof(req)) {
            fprintf(stderr, "Discarding malformed PDU of length %ld bytes\n", (long)n);
            continue;
        }

        switch (req.type) {
        case PDU_R: process_register   (s, &cli, clen, &req);  break;
        case PDU_S: process_search     (s, &cli, clen, &req);  break;
        case PDU_T: process_deregister (s, &cli, clen, &req);  break;
        case PDU_O: process_list       (s, &cli, clen);        break;
        default: {
            PDU e; reset_pdu(&e); e.type = PDU_E;
            sendto(s, &e, sizeof(e), 0, (const struct sockaddr*)&cli, clen);
        } break;
        }
    }
    return 0;
}
