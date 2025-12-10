// Standard networking headers for socket programming
#include <arpa/inet.h>     // inet_aton(), inet_ntoa(), htons(), ntohs()
#include <netinet/in.h>    // struct sockaddr_in, INADDR_ANY
#include <stdint.h>        
#include <stdio.h>         
#include <stdlib.h>       
#include <string.h>        
#include <sys/select.h>    // select(), fd_set for I/O multiplexing
#include <sys/socket.h>    
#include <sys/types.h>     // socklen_t, ssize_t type definitions
#include <unistd.h>        // close(), read(), write()
#include <errno.h>         // errno for error checking

// Protocol constants - must match index_server.c
#define PEER_NAME_LEN    10   // Maximum length for peer identifier
#define CONTENT_NAME_LEN 10   // Maximum length for content/file name
#define IP_STRLEN        16   // IPv4 address string length (xxx.xxx.xxx.xxx\0)

#define MAX_LISTEN       16   // Maximum simultaneous content registrations per peer

// PDU types (must match index_server.c) 
#define PDU_R  'R'  // Register content with index
#define PDU_S  'S'  // Search for content at index
#define PDU_T  'T'  // Deregister (terminate) content
#define PDU_O  'O'  // Request online content list
#define PDU_A  'A'  // Acknowledgment (success)
#define PDU_E  'E'  // Error response
#define PDU_D  'D'  // Download request (TCP)
#define PDU_C  'C'  // Content delivery header (TCP)

// Fixed-size protocol data unit exchanged with index via UDP
typedef struct __attribute__((packed)) {
    char     type;                      // PDU type character
    char     peer[PEER_NAME_LEN];      // Peer identifier (sender/target)
    char     content[CONTENT_NAME_LEN]; // Content name being registered/searched
    char     ip[IP_STRLEN];            // IP address for TCP connections
    uint16_t port_net;                 // TCP port in network byte order 
} PDU;

// Tracks one locally registered content item
typedef struct {
    int   in_use;                       // 1 if slot is active, 0 if free
    char  peer[PEER_NAME_LEN + 1];      // This peer's name (for consistency)
    char  content[CONTENT_NAME_LEN + 1]; // Content name being served
    int   listen_fd;                    // TCP socket listening for download requests
} LocalEntry;

// Global state: UDP channel to index and local content registry
static int udp_fd = -1;                         // UDP socket for all index communication
static struct sockaddr_in idx_addr;             // Index server address (cached from argv)
static char g_peer_name[PEER_NAME_LEN + 1];    // This peer's unique identifier
static char g_advertise_ip[IP_STRLEN];          // Optional: IP to advertise (for NAT/firewall)
static LocalEntry local_[MAX_LISTEN];           // Array of registered content items

// Zero out all fields of a PDU structure
static void init_pdu_clear(PDU *p) {
    memset(p, 0, sizeof(*p));
}

// Copy at most 'limit' chars from src to dst, zero-pad remainder
// Ensures fixed-length fields in PDUs are properly formatted
static void fill_field_padded(char *dst, size_t dsz, const char *src, size_t limit) {
    size_t n = 0;
    // Copy characters up to limit or null terminator
    while (n < limit && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    // Zero-pad the rest of the destination buffer
    while (n < dsz) {
        dst[n] = '\0';
        n++;
    }
}

// Consume remaining characters on current input line
// Prevents leftover input from affecting next scanf
static void drain_stdin_line(void) {
    int ch;
    do {
        ch = getchar();
    } while (ch != '\n' && ch != EOF);
}

// Discover this machine's outbound IP address
// Uses "connect" to a public IP to see which interface the kernel selects
static void detect_local_ip(char *buf) {
    int s;
    struct sockaddr_in tmp;
    socklen_t len;

    memset(buf, 0, IP_STRLEN); // Clear output buffer

    // Create temporary UDP socket for IP discovery
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        strncpy(buf, "127.0.0.1", IP_STRLEN - 1); // Fallback to localhost
        return;
    }

    memset(&tmp, 0, sizeof(tmp)); // Prepare target address
    tmp.sin_family = AF_INET; // IPv4
    tmp.sin_port   = htons(9); // Discard service (arbitrary)
    inet_aton("8.8.8.8", &tmp.sin_addr); // Google DNS as target (to detect routing)

    // Connect (doesn't send packets) to determine routing interface
    if (connect(s, (struct sockaddr *)&tmp, sizeof(tmp)) == 0) {
        len = (socklen_t)sizeof(tmp);
        // Query the local address kernel assigned for this route
        if (getsockname(s, (struct sockaddr *)&tmp, &len) == 0) {
            strncpy(buf, inet_ntoa(tmp.sin_addr), IP_STRLEN - 1);
            close(s);
            return;
        }
    }


    // Fallback if routing fails
    strncpy(buf, "127.0.0.1", IP_STRLEN - 1);
    close(s);
}

// Create a TCP listening socket on an OS-assigned ephemeral port
// Returns socket fd on success, -1 on error; sets *port_out to assigned port
static int open_content_listener(uint16_t *port_out) {
    int fd;
    struct sockaddr_in addr;
    socklen_t len;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(TCP)");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    addr.sin_port        = htons(0);          // 0 = OS picks available port

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { // Bind to ephemeral port
        perror("bind(TCP)");
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    len = (socklen_t)sizeof(addr);
    // Retrieve the actual port number OS assigned
    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        perror("getsockname");
        close(fd);
        return -1;
    }

    *port_out = ntohs(addr.sin_port);
    return fd;
}

// Send a PDU to index server and wait for one reply (with 2s timeout)
// Returns 0 on success, -1 on timeout/error
static int send_pdu_wait_reply(const PDU *out, PDU *in) {
    ssize_t n;
    struct timeval tv;
    fd_set rfds;

    // Transmit request PDU to index server
    n = sendto(udp_fd, out, sizeof(*out), 0,
               (struct sockaddr *)&idx_addr, sizeof(idx_addr));
    if (n != (ssize_t)sizeof(*out)) {
        perror("sendto");
        return -1;
    }

    // Set up 2-second timeout for reply
    FD_ZERO(&rfds);
    FD_SET(udp_fd, &rfds);
    tv.tv_sec  = 2;
    tv.tv_usec = 0;

    // Wait for response or timeout
    if (select(udp_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
        if (errno != 0) perror("select(udp)");
        return -1;
    }

    // Receive reply PDU from index
    n = recvfrom(udp_fd, in, sizeof(*in), 0, NULL, NULL);
    if (n != (ssize_t)sizeof(*in)) {
        fprintf(stderr, "Short/long UDP reply (%ld bytes)\n", (long)n);
        return -1;
    }

    return 0;
}

// Accept and handle one incoming download request on a TCP listener
// Protocol: receive 'D' + content_name, respond with 'C' + file data or 'E'
static void handle_single_download(int listen_fd) {
    int cfd;
    struct sockaddr_in cli;
    socklen_t clen;
    char typ;
    char namebuf[CONTENT_NAME_LEN];
    char fname[CONTENT_NAME_LEN + 16];
    FILE *fp;
    char buf[4096];
    size_t n;

    clen = (socklen_t)sizeof(cli); // Accept incoming connection
    cfd = accept(listen_fd, (struct sockaddr *)&cli, &clen); // Accept connection
    if (cfd < 0) {
        perror("accept");
        return;
    }

    // Phase 1: Read download request header ('D')
    if (read(cfd, &typ, 1) != 1 || typ != PDU_D) {
        close(cfd);
        return;
    }

    // Phase 2: Read requested content name (10 bytes, possibly padded)
    if (read(cfd, namebuf, CONTENT_NAME_LEN) != CONTENT_NAME_LEN) {
        close(cfd);
        return;
    }

    // Extract null-terminated filename from fixed-width buffer
    {
        int k;
        for (k = 0; k < CONTENT_NAME_LEN; ++k) {
            if (namebuf[k] == '\0' || namebuf[k] == ' ')
                break;
            fname[k] = namebuf[k];
        }
        fname[k] = '\0';
    }

    // Phase 3: Attempt to open requested file
    fp = fopen(fname, "rb");
    if (!fp) {
        // File not found: send error response and close
        typ = PDU_E;
        (void)write(cfd, &typ, 1);
        close(cfd);
        return;
    }

    // Phase 4: Send success header
    typ = PDU_C;
    (void)write(cfd, &typ, 1);

    // Phase 5: Stream file contents to requester
    for (;;) {
        n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break; // EOF reached
        if (write(cfd, buf, n) < 0) break; // Connection closed
    }

    fclose(fp);
    close(cfd);
}

// Menu action R: Register locally available content with the index server
// Creates a TCP listener for downloads and notifies index of availability
static void cmd_register_content(void) {
    char content[CONTENT_NAME_LEN + 1];
    char filename[128];
    uint16_t port;
    int listen_fd;
    char myip[IP_STRLEN];
    PDU r;
    PDU ans;
    int i;

    memset(content, 0, sizeof(content));
    memset(filename, 0, sizeof(filename));

    // Step 1: Prompt user for content name
    printf("Content tag (max %d chars): ", CONTENT_NAME_LEN);
    if (scanf("%10s", content) != 1) {
        puts("Invalid content name.");
        drain_stdin_line();
        return;
    }
    drain_stdin_line();

    // Step 2: Prompt for local filename
    printf("Filename on disk to share: ");
    if (scanf("%127s", filename) != 1) {
        puts("Invalid filename.");
        drain_stdin_line();
        return;
    }
    drain_stdin_line();

    // Constraint: For simplicity, filename must match content name
    if (strcmp(filename, content) != 0) {
        puts("For this peer implementation, filename must equal the content name.");
        return;
    }

    // Step 3: Create TCP listener for serving this content
    port = 0;
    listen_fd = open_content_listener(&port);
    if (listen_fd < 0) {
        return;
    }

    // Step 4: Determine IP to advertise (manual override or auto-detect)
    memset(myip, 0, sizeof(myip));
    if (g_advertise_ip[0] != '\0') {
        // Use manually specified IP (for NAT scenarios)
        strncpy(myip, g_advertise_ip, IP_STRLEN - 1);
    } else {
        // Auto-detect local IP via routing trick
        detect_local_ip(myip);
    }

    // Step 5: Build registration PDU
    init_pdu_clear(&r);
    r.type = PDU_R;
    fill_field_padded(r.peer,    sizeof(r.peer),    g_peer_name, PEER_NAME_LEN);
    fill_field_padded(r.content, sizeof(r.content), content,     CONTENT_NAME_LEN);
    fill_field_padded(r.ip,      sizeof(r.ip),      myip,        IP_STRLEN - 1);
    r.port_net = htons(port);

    // Step 6: Send registration to index and await acknowledgment
    if (send_pdu_wait_reply(&r, &ans) == 0) {
        if (ans.type == PDU_A) {
            // Success: store in local table to handle future download requests
            for (i = 0; i < MAX_LISTEN; ++i) {
                if (!local_[i].in_use) {
                    local_[i].in_use = 1;
                    fill_field_padded(local_[i].peer,    sizeof(local_[i].peer),
                              g_peer_name, PEER_NAME_LEN);
                    fill_field_padded(local_[i].content, sizeof(local_[i].content),
                              content, CONTENT_NAME_LEN);
                    local_[i].listen_fd = listen_fd;
                    printf("Now serving '%s' from %s:%u (listener fd=%d)\n",
                           content, myip, port, listen_fd);
                    break;
                }
            }
            if (i == MAX_LISTEN) {
                puts("Local table full; closing listener.");
                close(listen_fd);
            }
        } else if (ans.type == PDU_E) {
            // Index rejected (duplicate peer/content pair)
            puts("Registration rejected by index: this peer name already registered that content.");
            puts("Please choose a different peer name before registering this content.");
            close(listen_fd);
        } else {
            puts("Registration failed (unexpected reply from index).");
            close(listen_fd);
        }
    } else {
        // No response from index (timeout/network issue)
        puts("Could not reach the index server (no UDP reply).");
        close(listen_fd);
    }
}

// Menu action S: Search for content, download it via TCP, then auto-register as provider
// Three-phase operation: query index → download from peer → become provider yourself
static void cmd_search_and_fetch(void) {
    char content[CONTENT_NAME_LEN + 1];
    PDU sreq;
    PDU ans;
    uint16_t tcp_port;
    struct sockaddr_in a;
    int cfd;
    char padname[CONTENT_NAME_LEN];
    char header;
    char outname[64];
    FILE *fp;
    char buf[4096];
    ssize_t n;
    uint32_t total = 0;

    PDU r;
    PDU ack;
    char myip[IP_STRLEN];
    uint16_t port;
    int listen_fd;
    int i;

    memset(content, 0, sizeof(content));

    // Step 1: Get content name from user
    printf("Type the content tag you want to look up and download: ");
    if (scanf("%10s", content) != 1) {
        puts("Invalid content name.");
        drain_stdin_line();
        return;
    }
    drain_stdin_line();

    // Phase 1: Query index for content provider
    init_pdu_clear(&sreq);
    sreq.type = PDU_S;
    fill_field_padded(sreq.peer,    sizeof(sreq.peer),    g_peer_name, PEER_NAME_LEN);
    fill_field_padded(sreq.content, sizeof(sreq.content), content,     CONTENT_NAME_LEN);

    if (send_pdu_wait_reply(&sreq, &ans) != 0) {
        puts("No response from index (check IP/port).");
        return;
    }

    if (ans.type == PDU_E) {
        puts("Content not found on any peer.");
        return;
    }
    if (ans.type != PDU_S) {
        puts("Unexpected response type from index.");
        return;
    }

    // Phase 2: Connect to provider and download content via TCP
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = ans.port_net; // Already in network byte order
    if (inet_aton(ans.ip, &a.sin_addr) == 0) {
        puts("Bad IP address from index.");
        return;
    }

    tcp_port = ntohs(ans.port_net);
    printf("Index chose provider %s:%u for this download\n", ans.ip, tcp_port);
    printf("Opening TCP connection to provider %s:%u ...\n", ans.ip, tcp_port);

    // Create TCP socket and connect to content provider
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) {
        perror("socket(TCP)");
        return;
    }

    if (connect(cfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("connect");
        close(cfd);
        return;
    }
    // Set a 5-second receive timeout on the TCP socket
    // If no data is received for 5 seconds, reads will fail with EAGAIN/EWOULDBLOCK.
    {
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        if (setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            perror("setsockopt(SO_RCVTIMEO)");
            // Can still try downloading without the timeout
        }
    }

    // Send download request: 'D' + content_name
    header = PDU_D;
    if (write(cfd, &header, 1) != 1) {
        perror("write(D)");
        close(cfd);
        return;
    }

    // Send content name (zero-padded to CONTENT_NAME_LEN)
    memset(padname, 0, sizeof(padname));
    memcpy(padname, content,
           strlen(content) < CONTENT_NAME_LEN ? strlen(content) : CONTENT_NAME_LEN);

    if (write(cfd, padname, CONTENT_NAME_LEN) != CONTENT_NAME_LEN) {
        perror("write(name)");
        close(cfd);
        return;
    }

    // Read response header from server
    if (read(cfd, &header, 1) != 1) {
        puts("No header from content server.");
        close(cfd);
        return;
    }

    if (header == PDU_E) {
        puts("Content server reported: file not found.");
        close(cfd);
        return;
    }

    if (header != PDU_C) {
        puts("Unexpected header from content server.");
        close(cfd);
        return;
    }

    // Open local file to save downloaded content
    snprintf(outname, sizeof(outname), "recv_%s", content);
    fp = fopen(outname, "wb");
    if (!fp) {
        perror("fopen(recv_*)");
        close(cfd);
        return;
    }

    // Stream content from TCP connection to local file
    for (;;) {
        n = read(cfd, buf, sizeof(buf));
        if (n < 0) {
            perror("read");
            break;
        }
        if (n == 0) break; // EOF
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            perror("fwrite");
            break;
        }
        total += (uint32_t)n;
    }

    fclose(fp);
    close(cfd);

    printf("Finished download: %u bytes saved as '%s'.\n", total, outname);
    if (total == 0) {
        puts("Warning: downloaded 0 bytes – check that the server file is non-empty.");
    }

    // Phase 3: Auto-register as content provider for load distribution
    port = 0;
    listen_fd = open_content_listener(&port);
    if (listen_fd < 0) {
        return;
    }

    // Determine IP to advertise
    memset(myip, 0, sizeof(myip));
    if (g_advertise_ip[0] != '\0') {
        strncpy(myip, g_advertise_ip, IP_STRLEN - 1);
    } else {
        detect_local_ip(myip);
    }

    // Build auto-registration PDU
    init_pdu_clear(&r);
    r.type = PDU_R;
    fill_field_padded(r.peer,    sizeof(r.peer),    g_peer_name, PEER_NAME_LEN);
    fill_field_padded(r.content, sizeof(r.content), content,     CONTENT_NAME_LEN);
    fill_field_padded(r.ip,      sizeof(r.ip),      myip,        IP_STRLEN - 1);
    r.port_net = htons(port);

    // Send auto-registration to index
    if (send_pdu_wait_reply(&r, &ack) == 0) {
        if (ack.type == PDU_A) {
            // Success: add to local table to serve future requests
            for (i = 0; i < MAX_LISTEN; ++i) {
                if (!local_[i].in_use) {
                    local_[i].in_use = 1;
                    fill_field_padded(local_[i].peer,    sizeof(local_[i].peer),
                              g_peer_name, PEER_NAME_LEN);
                    fill_field_padded(local_[i].content, sizeof(local_[i].content),
                              content, CONTENT_NAME_LEN);
                    local_[i].listen_fd = listen_fd;
                    printf("[auto] Registered '%s' at %s:%u\n", content, myip, port);
                    break;
                }
            }
            if (i == MAX_LISTEN) {
                close(listen_fd);
            }
        } else if (ack.type == PDU_E) {
            puts("[auto] Registration rejected by index for this content/peer name.");
            close(listen_fd);
        } else {
            puts("[auto] Registration failed (unexpected reply from index).");
            close(listen_fd);
        }
    } else {
        puts("[auto] Could not reach the index server (no UDP reply).");
        close(listen_fd);
    }
}

// Menu action T: Deregister one content item from index and close its TCP listener
// Removes entry from both index server and local table
static void cmd_deregister_content(void) {
    char content[CONTENT_NAME_LEN + 1];
    PDU t;
    PDU ans;
    int i;

    memset(content, 0, sizeof(content));

    // Step 1: Prompt for content name to deregister
    printf("Content tag to stop serving: ");
    if (scanf("%10s", content) != 1) {
        puts("Invalid content name.");
        drain_stdin_line();
        return;
    }
    drain_stdin_line();

    // Step 2: Find content in local table
    for (i = 0; i < MAX_LISTEN; ++i) {
        if (!local_[i].in_use) continue;
        if (strncmp(local_[i].content, content, CONTENT_NAME_LEN) == 0) {
            break;
        }
    }

    if (i == MAX_LISTEN) {
        puts("No such content registered locally.");
        return;
    }

    // Step 3: Build deregistration PDU
    init_pdu_clear(&t);
    t.type = PDU_T;
    fill_field_padded(t.peer,    sizeof(t.peer),    g_peer_name, PEER_NAME_LEN);
    fill_field_padded(t.content, sizeof(t.content), content,     CONTENT_NAME_LEN);

    // Step 4: Send deregistration request to index
    if (send_pdu_wait_reply(&t, &ans) == 0 && ans.type == PDU_A) {
        printf("Deregistered '%s' from index.\n", content);
        // Clean up: close TCP listener and free table slot
        close(local_[i].listen_fd);
        local_[i].listen_fd = -1;
        local_[i].in_use = 0;
    } else {
        puts("Deregister failed (index did not ack).");
    }
}

// Menu action O: Request and display list of all online content from index
// Index streams multiple PDUs (one per content item) followed by empty terminator
static void cmd_show_online(void) {
    PDU o;
    PDU row;
    ssize_t n;

    printf("Catalogue reported by index (one line per active entry):\n");

    // Step 1: Send list request to index
    init_pdu_clear(&o);
    o.type = PDU_O;

    n = sendto(udp_fd, &o, sizeof(o), 0,
               (struct sockaddr *)&idx_addr, sizeof(idx_addr));
    if (n != (ssize_t)sizeof(o)) {
        perror("send(O)");
        return;
    }

    // Step 2: Receive and display content entries until terminator
    for (;;) {
        n = recvfrom(udp_fd, &row, sizeof(row), 0, NULL, NULL);
        if (n != (ssize_t)sizeof(row)) {
            fprintf(stderr, "Short/long O row (%ld bytes)\n", (long)n);
            return;
        }

        if (row.type != PDU_O) {
            return;
        }

        // Empty peer field signals end of list
        if (row.peer[0] == '\0') {
            break;
        }

        // Display content entry
        printf("  Peer=%.*s  Content=%.*s  Addr=%s:%u\n",
               PEER_NAME_LEN, row.peer,
               CONTENT_NAME_LEN, row.content,
               row.ip, (unsigned)ntohs(row.port_net));
    }
}

// Display interactive menu of available commands
static void show_peer_menu(void) {
    printf("\n=== P2P Peer Console ===\n");
    printf("R : Share a local file with the network\n");
    printf("S : Locate a file and fetch it from another peer\n");
    printf("O : Show the index's list of advertised content\n");
    printf("T : Stop sharing one advertised file\n");
    printf("Q : Remove everything you share and exit\n");
    printf("Select option (R/S/O/T/Q): ");
    fflush(stdout);
}

// Main entry point: parse arguments, initialize UDP channel, run event loop
int main(int argc, char *argv[]) {
    int i;
    char line[32];
    fd_set rfds;
    int maxfd;

    // Validate command-line arguments
    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "Usage: %s <index_ip> <index_udp_port> [advertise_ip]\n",
                argv[0]);
        return 1;
    }

    // Initialize global state
    memset(g_peer_name, 0, sizeof(g_peer_name));
    memset(g_advertise_ip, 0, sizeof(g_advertise_ip));
    memset(local_, 0, sizeof(local_));

    // Optional: use manually specified IP for NAT scenarios
    if (argc == 4) {
        strncpy(g_advertise_ip, argv[3], IP_STRLEN - 1);
    }

    // Prompt for peer identifier (used in all registrations)
    printf("Choose a peer id (<=%d chars): ", PEER_NAME_LEN);
    if (scanf("%10s", g_peer_name) != 1) {
        fprintf(stderr, "Invalid peer name.\n");
        drain_stdin_line();
        return 1;
    }
    drain_stdin_line();

    // Create UDP socket for index communication
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("socket(UDP)");
        return 1;
    }

    // Parse and store index server address
    memset(&idx_addr, 0, sizeof(idx_addr));
    idx_addr.sin_family = AF_INET;
    idx_addr.sin_port   = htons((uint16_t)atoi(argv[2]));
    if (inet_aton(argv[1], &idx_addr.sin_addr) == 0) {
        fprintf(stderr, "Bad index IP address: %s\n", argv[1]);
        close(udp_fd);
        return 1;
    }

    printf("Peer '%s' is up. Talking to index at %s:%s\n",
           g_peer_name, argv[1], argv[2]);

    // Main event loop: multiplex between user input and incoming download requests
    for (;;) {
        int ready;
        show_peer_menu();

        // Build file descriptor set: stdin + all active TCP listeners
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        maxfd = STDIN_FILENO;

        // Add all registered content listeners to the select set
        for (i = 0; i < MAX_LISTEN; ++i) {
            if (local_[i].in_use && local_[i].listen_fd >= 0) {
                FD_SET(local_[i].listen_fd, &rfds);
                if (local_[i].listen_fd > maxfd) {
                    maxfd = local_[i].listen_fd;
                }
            }
        }

        // Block until stdin or a TCP listener has activity
        ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select");
            break;
        }

        // Handle user input if stdin is ready
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                // EOF (Ctrl+D): exit gracefully
                break;
            }

            if (line[0] == '\n' || line[0] == '\0') {
                continue; // Blank line: redisplay menu
            }

            // Dispatch menu command
            switch (line[0]) {
            case 'R':
            case 'r':
                cmd_register_content();
                break;
            case 'S':
            case 's':
                cmd_search_and_fetch();
                break;
            case 'O':
            case 'o':
                cmd_show_online();
                break;
            case 'T':
            case 't':
                cmd_deregister_content();
                break;
            case 'Q':
            case 'q':
                // Graceful shutdown: deregister all content before exit
                for (i = 0; i < MAX_LISTEN; ++i) {
                    if (local_[i].in_use) {
                        PDU t;
                        PDU ans;
                        init_pdu_clear(&t);
                        t.type = PDU_T;
                        fill_field_padded(t.peer,    sizeof(t.peer),    g_peer_name, PEER_NAME_LEN);
                        fill_field_padded(t.content, sizeof(t.content),
                                  local_[i].content, CONTENT_NAME_LEN);
                        (void)send_pdu_wait_reply(&t, &ans); // Best-effort deregister
                        if (local_[i].listen_fd >= 0) {
                            close(local_[i].listen_fd);
                        }
                        local_[i].listen_fd = -1;
                        local_[i].in_use = 0;
                    }
                }
                printf("Shutting down peer and deregistering any remaining content.\n");
                close(udp_fd);
                return 0;
            default:
                puts("Unknown choice.");
                break;
            }
        }

        // Handle incoming download requests on any active listener
        for (i = 0; i < MAX_LISTEN; ++i) {
            if (local_[i].in_use &&
                local_[i].listen_fd >= 0 &&
                FD_ISSET(local_[i].listen_fd, &rfds)) { // Incoming request
                handle_single_download(local_[i].listen_fd); // Handle one download
            }
        }
    }

    // Clean up on abnormal exit
    close(udp_fd);
    return 0;
}
