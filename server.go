package main

import (
	"context"
	"errors"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"time"

	"github.com/coder/websocket"
)

type server struct {
}

func main() {
	e := run()
	if e != nil {
		log.Fatal(e)
	}
}

// run starts a http.Server for the passed in address
// with all requests handled by echoServer.
func run() error {
	if len(os.Args) < 2 {
		return errors.New("please provide an address to listen on as the first argument")
	}

	tlsListener, err := net.Listen("tcp", os.Args[1])
	if err != nil {
		return err
	}

	log.Printf("listening on https://%v", tlsListener.Addr())

	tlsServer := &http.Server{
		Handler:      server{},
		ReadTimeout:  time.Second * 10,
		WriteTimeout: time.Second * 10,
	}

	// certFile := "server.crt"
	// keyFile := "server.key"
	errc := make(chan error, 1)
	go func() {
		// errc <- tlsServer.ServeTLS(tlsListener, certFile, keyFile)
		errc <- tlsServer.Serve(tlsListener)
	}()

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, os.Interrupt)
	select {
	case err := <-errc:
		log.Printf("failed to serve: %v", err)
	case sig := <-sigs:
		log.Printf("terminating: %v", sig)
	}

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*10)
	defer cancel()

	return tlsServer.Shutdown(ctx)
}

func (s server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	c, err := websocket.Accept(w, r, nil)
	if err != nil {
		log.Println("Error upgrading websocket connection:", err)
		return
	}
	defer c.Close(websocket.StatusInternalError, "the sky is falling")

	Echo(c, r)
}

func Echo(c *websocket.Conn, r *http.Request) {
	for {
		ty, data, err := c.Read(r.Context())
		if err != nil {
			log.Printf("Failed to read from %v: %v", r.RemoteAddr, err)
			break
		}
		strData := string(data)

		if strData == "/CLOSE" {
			err = c.Close(websocket.StatusNormalClosure, "Close command")
			if err != nil {
				log.Printf("Failed to gracefully close connection %v: %v", r.RemoteAddr, err)
			}
			break
		}
		if strings.HasPrefix(strData, "/HEADER ") {
			data = []byte(r.Header.Get(strings.TrimPrefix(strData, "/HEADER ")))
		}

		err = c.Write(r.Context(), ty, data)
		if err != nil {
			log.Printf("Failed to write to %v: %v", r.RemoteAddr, err)
			break
		}
	}
}
