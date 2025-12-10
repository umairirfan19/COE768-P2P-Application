# **P2P Content Sharing System (COE768 – Computer Networks)**  

A lightweight peer-to-peer (P2P) content sharing system that uses:

- **UDP** for directory lookups (Index Server)  
- **TCP** for reliable file transfer between peers  

This project demonstrates socket programming concepts such as event-driven I/O, connection management, custom fixed-size PDUs, and load-balanced content distribution.

---

## 🔍 **Overview**

The system consists of:

### **1. Index Server (`index.c`)**
A minimal directory service that:
- Stores active registrations `{peer, content, IP, TCP port}`
- Responds to peer search requests
- Tracks how many times each peer has served a file (`use_count`)
- Balances load by returning the least-used peer for each content

Communication is entirely over **UDP** using fixed-size PDUs.

---

### **2. Peer Application (`peer.c`)**
Each peer acts as **both** a client and a server.

#### Peer Responsibilities:
- Register content with the index server  
- Search for content  
- Download content via TCP  
- Automatically become a content server after downloading  
- Serve incoming download requests from other peers  
- Maintain an interactive menu while simultaneously accepting TCP connections  
  (using `select()` for multiplexing)

---

## 📦 **Protocol Summary**

The system uses fixed-size PDUs with the following types:

| PDU | Meaning |
|-----|---------|
| `R` | Register content with index |
| `S` | Search for content provider |
| `T` | Deregister content |
| `O` | Request full online content list |
| `A` | Acknowledgement (success) |
| `E` | Error |
| `D` | Download request (TCP) |
| `C` | Content data (TCP) |

Each PDU includes:
- Peer name (10 bytes)  
- Content name (10 bytes)  
- IP address (fixed 16-byte string)  
- TCP port (uint16_t, network order)

---

## ⚙️ **Key Features**

### ✔ **Directory Built on UDP**
Fast, connectionless messaging for:
- Registration  
- De-registration  
- Searching  
- Listing all active content  

### ✔ **Reliable File Transfer over TCP**
Peers communicate directly to transfer actual file data.

### ✔ **Load-Balanced Content Serving**
The index server returns the peer with the **lowest `use_count`** to avoid overloading a single provider.

### ✔ **Event-Driven Peer Design**
The peer uses `select()` to monitor:
- `stdin` for user input  
- Multiple TCP listening sockets (one per file being served)

This allows full concurrency **without multithreading**.

### ✔ **Automatic Replication**
After a successful download:
1. The peer stores the file locally  
2. It registers itself as a new provider of that content  
3. The number of available providers increases automatically  
4. Overall network resilience improves  

---

## ▶️ **How to Run**

### **1. Start the Index Server**
```bash
gcc index.c -o index
./index <udp_port>
```

### **2. Start Each Peer**
```bash
gcc peer.c -o peer
./peer <index_ip> <index_port>
```

### **3. Use the Menu to:**
- Register a file  
- Search and download  
- List all available content  
- Deregister  
- Quit (auto-cleanup)

---

## 🛠️ **Project Structure**

```
/
├── index.c        # UDP-based directory server
├── peer.c         # Peer client/server logic with TCP downloads
└── README.md      # Project documentation
```

---

## 📘 **Concepts Demonstrated**
- UDP and TCP socket programming  
- Custom application-layer protocol design  
- Fixed-size PDU encoding  
- Load balancing using usage counters  
- I/O multiplexing (`select()` system call)  
- Single-threaded concurrency  
- Dynamic TCP port assignment using `getsockname()`  

---

## 📄 **Notes**
- No proprietary course material or partner-written text is included.  
- README summarizes the system based on the implementation itself.  
- You may extend this repo with diagrams, examples, or sample runs later.

