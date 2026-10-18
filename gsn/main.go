package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

func main() {
	path := flag.String("config", "config/gsn.json", "configuration file")
	flag.Parse()
	config, err := loadConfig(*path)
	if err != nil {
		log.Fatal("GSN configuration: ", err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	app := NewServer(config)
	if app.router != nil {
		app.router.credentialsPath = filepath.Join(filepath.Dir(*path), "router-login.private.json")
	}
	go app.Run(ctx)
	server := &http.Server{Addr: config.Listen, Handler: app, ReadHeaderTimeout: 2 * time.Second, ReadTimeout: 2 * time.Second, WriteTimeout: 2 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 4096}
	go func() {
		<-ctx.Done()
		c, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		server.Shutdown(c)
	}()
	log.Print("GSO STATUS NAS listening on ", config.Listen)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal("GSN HTTP server failed: ", err)
	}
}
