// deed gRPC test server — REFACTOR_SPEC e2e backend for the new domain stack's gRPC path.
// Reflection-enabled echo service so DEED's reflection (GrpcSender) can discover + call it.
//
//   echo.Echo/Unary(EchoRequest{msg,count})        -> EchoResponse{msg}              (echoes msg once)
//   echo.Echo/ServerStream(EchoRequest{msg,count}) -> stream EchoResponse{msg#i}     (count echoes)
//   echo.Echo/ClientStream(stream EchoRequest)     -> EchoResponse{msg}              (joins all msgs with ",")
//   echo.Echo/BiDi(stream EchoRequest)             -> stream EchoResponse{msg}       (echoes each back)
//
// Usage: grpcserver [port]  (prints "LISTENING <port>" once bound, then serves until killed)
package main

import (
	"context"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"

	"grpcserver/pb"

	"google.golang.org/grpc"
	"google.golang.org/grpc/reflection"
)

type echoServer struct{ pb.UnimplementedEchoServer }

func (s *echoServer) Unary(_ context.Context, r *pb.EchoRequest) (*pb.EchoResponse, error) {
	return &pb.EchoResponse{Msg: r.GetMsg()}, nil
}

func (s *echoServer) ServerStream(r *pb.EchoRequest, stream pb.Echo_ServerStreamServer) error {
	n := r.GetCount()
	if n <= 0 {
		n = 1
	}
	for i := int32(0); i < n; i++ {
		if err := stream.Send(&pb.EchoResponse{Msg: fmt.Sprintf("%s#%d", r.GetMsg(), i)}); err != nil {
			return err
		}
	}
	return nil
}

// ClientStream collects every request msg and returns them joined with "," (so the client can assert all
// N messages arrived, in order).
func (s *echoServer) ClientStream(stream pb.Echo_ClientStreamServer) error {
	var msgs []string
	for {
		r, err := stream.Recv()
		if err == io.EOF {
			return stream.SendAndClose(&pb.EchoResponse{Msg: strings.Join(msgs, ",")})
		}
		if err != nil {
			return err
		}
		msgs = append(msgs, r.GetMsg())
	}
}

// BiDi echoes each received request msg straight back as a response (ping-pong).
func (s *echoServer) BiDi(stream pb.Echo_BiDiServer) error {
	for {
		r, err := stream.Recv()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if err := stream.Send(&pb.EchoResponse{Msg: r.GetMsg()}); err != nil {
			return err
		}
	}
}

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
	srv := grpc.NewServer()
	pb.RegisterEchoServer(srv, &echoServer{})
	reflection.Register(srv) // DEED discovers methods via server reflection

	fmt.Printf("LISTENING %d\n", ln.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()
	_ = srv.Serve(ln)
}
