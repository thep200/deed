// deed test server — REFACTOR_SPEC e2e backend for the new domain stack (CoreApiClient).
// Pure Go standard library (no external modules, builds offline). One process serves every request type the
// app sends, on a single ephemeral port:
//
//   HTTP   any non-upgrade request          -> 200 JSON echo {method,path,x_test,body}
//   GraphQL POST /graphql                    -> 200 {"data":{"echo":<query>,"variables":<vars>}}
//   SSE    GET /sse?count=N                  -> text/event-stream of N `data: msg#i` events
//   GraphQL-ws  /graphqlws (Upgrade)          -> graphql-transport-ws: init->ack, subscribe->3x next->complete
//   WebSocket (Upgrade: websocket)           -> RFC6455 echo (text/binary echoed; ping->pong; close->close)
//
// Usage: testserver [port]   (prints "LISTENING <port>" once bound, then serves until killed)
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
)

const wsMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

func main() {
	port := 0
	if len(os.Args) > 1 {
		port, _ = strconv.Atoi(os.Args[1])
	}
	ln, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(port))
	if err != nil {
		fmt.Fprintln(os.Stderr, "listen error:", err)
		os.Exit(1)
	}
	fmt.Printf("LISTENING %d\n", ln.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()

	mux := http.NewServeMux()
	mux.HandleFunc("/", root)
	mux.HandleFunc("/graphql", graphqlHandler)
	mux.HandleFunc("/sse", sseHandler)
	mux.HandleFunc("/graphqlws", graphqlWsHandler)
	_ = http.Serve(ln, mux)
}

func root(w http.ResponseWriter, r *http.Request) {
	if strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		wsEcho(w, r)
		return
	}
	httpEcho(w, r)
}

// ---- HTTP echo ----------------------------------------------------------------------------------
func httpEcho(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(r.Body)
	resp := map[string]string{
		"method":        r.Method,
		"path":          r.URL.Path,
		"x_test":        r.Header.Get("X-Test"),
		"authorization": r.Header.Get("Authorization"), // assert native senders apply (resolved) auth
		"body":          string(body),
	}
	out, _ := json.Marshal(resp)
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("X-Echo", "1")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(out)
}

// ---- GraphQL ------------------------------------------------------------------------------------
func graphqlHandler(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(r.Body)
	var req struct {
		Query     string          `json:"query"`
		Variables json.RawMessage `json:"variables"`
	}
	_ = json.Unmarshal(body, &req)
	vars := req.Variables
	if len(vars) == 0 {
		vars = json.RawMessage("{}")
	}
	data, _ := json.Marshal(map[string]any{
		"data": map[string]any{
			"echo":          req.Query,
			"variables":     json.RawMessage(vars),
			"authorization": r.Header.Get("Authorization"), // assert native GraphQL sender applies auth
		},
	})
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

// ---- SSE (text/event-stream) --------------------------------------------------------------------
// Emits `count` events (default 3) as `data: msg#<i>\n\n`, then returns (closing the stream). A real
// SSE client reconnects on a clean close (EventSource semantics) — the e2e receives the events then
// cancels, so the reconnect backoff is never waited out.
func sseHandler(w http.ResponseWriter, r *http.Request) {
	count := 3
	if c := r.URL.Query().Get("count"); c != "" {
		if n, err := strconv.Atoi(c); err == nil && n > 0 {
			count = n
		}
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	fl, _ := w.(http.Flusher)
	for i := 0; i < count; i++ {
		fmt.Fprintf(w, "data: msg#%d\n\n", i)
		if fl != nil {
			fl.Flush()
		}
	}
}

// ---- WebSocket (RFC 6455, minimal echo) ---------------------------------------------------------
func wsEcho(w http.ResponseWriter, r *http.Request) {
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing key", http.StatusBadRequest)
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "no hijack", http.StatusInternalServerError)
		return
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		return
	}
	defer conn.Close()

	sum := sha1.Sum([]byte(key + wsMagic))
	accept := base64.StdEncoding.EncodeToString(sum[:])
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := brw.WriteString(resp); err != nil {
		return
	}
	if err := brw.Flush(); err != nil {
		return
	}

	// Auth probe: if the handshake carried an Authorization header (native WS sender applied resolved auth),
	// send it back as the FIRST text frame so the e2e can assert it. No header -> no extra frame (existing
	// no-auth tests are unaffected).
	if auth := r.Header.Get("Authorization"); auth != "" {
		probe, _ := json.Marshal(map[string]string{"authorization": auth})
		_ = writeFrame(brw, 0x1, probe)
		_ = brw.Flush()
	}

	for {
		opcode, payload, err := readFrame(brw.Reader)
		if err != nil {
			return
		}
		switch opcode {
		case 0x8: // close -> echo close + done
			_ = writeFrame(brw, 0x8, payload)
			_ = brw.Flush()
			return
		case 0x9: // ping -> pong
			_ = writeFrame(brw, 0xA, payload)
			_ = brw.Flush()
		case 0x1, 0x2: // text / binary -> echo
			_ = writeFrame(brw, opcode, payload)
			_ = brw.Flush()
		}
	}
}

// ---- GraphQL-over-WebSocket subscription (graphql-transport-ws) ---------------------------------
// Handshake echoes the offered subprotocol, then: connection_init -> connection_ack; subscribe/start ->
// 3x next (payload {"data":{"n":i}}) -> complete. Mirrors the client's GraphQlWsProtocol state machine.
func graphqlWsHandler(w http.ResponseWriter, r *http.Request) {
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing key", http.StatusBadRequest)
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "no hijack", http.StatusInternalServerError)
		return
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		return
	}
	defer conn.Close()

	sum := sha1.Sum([]byte(key + wsMagic))
	accept := base64.StdEncoding.EncodeToString(sum[:])
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n"
	if sub := r.Header.Get("Sec-WebSocket-Protocol"); sub != "" {
		resp += "Sec-WebSocket-Protocol: " + strings.TrimSpace(strings.Split(sub, ",")[0]) + "\r\n"
	}
	resp += "\r\n"
	if _, err := brw.WriteString(resp); err != nil {
		return
	}
	if err := brw.Flush(); err != nil {
		return
	}

	sendText := func(v any) error {
		b, _ := json.Marshal(v)
		if e := writeFrame(brw, 0x1, b); e != nil {
			return e
		}
		return brw.Flush()
	}
	for {
		opcode, payload, err := readFrame(brw.Reader)
		if err != nil {
			return
		}
		switch opcode {
		case 0x8:
			_ = writeFrame(brw, 0x8, payload)
			_ = brw.Flush()
			return
		case 0x9:
			_ = writeFrame(brw, 0xA, payload)
			_ = brw.Flush()
		case 0x1:
			var m map[string]any
			if json.Unmarshal(payload, &m) != nil {
				continue
			}
			switch m["type"] {
			case "connection_init":
				_ = sendText(map[string]any{"type": "connection_ack"})
			case "subscribe", "start":
				id := m["id"]
				for i := 0; i < 3; i++ {
					_ = sendText(map[string]any{"id": id, "type": "next",
						"payload": map[string]any{"data": map[string]any{"n": i}}})
				}
				_ = sendText(map[string]any{"id": id, "type": "complete"})
			}
		}
	}
}

func readFrame(r *bufio.Reader) (byte, []byte, error) {
	b0, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	opcode := b0 & 0x0f
	b1, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	masked := b1&0x80 != 0
	length := uint64(b1 & 0x7f)
	switch length {
	case 126:
		var ext [2]byte
		if _, err := io.ReadFull(r, ext[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err := io.ReadFull(r, ext[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(ext[:])
	}
	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(r, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return opcode, payload, nil
}

func writeFrame(w io.Writer, opcode byte, payload []byte) error {
	header := []byte{0x80 | opcode}
	n := len(payload)
	switch {
	case n < 126:
		header = append(header, byte(n))
	case n < 65536:
		header = append(header, 126, byte(n>>8), byte(n))
	default:
		ext := make([]byte, 8)
		binary.BigEndian.PutUint64(ext, uint64(n))
		header = append(header, 127)
		header = append(header, ext...)
	}
	if _, err := w.Write(header); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}
