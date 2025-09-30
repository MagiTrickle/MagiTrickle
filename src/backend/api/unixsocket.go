package api

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"

	"magitrickle/api/v1"
	"magitrickle/app"
	"magitrickle/constant"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/rs/zerolog/log"
)

func SetupUnixSocket(a app.Main, errChan chan error) (*http.Server, error) {
	if err := os.Remove(constant.DefaultSocketPath); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("failed to remove existing UNIX socket: %w", err)
	}

	socket, err := net.Listen("unix", constant.DefaultSocketPath)
	if err != nil {
		return nil, fmt.Errorf("error while serving UNIX socket: %v", err)
	}

	r := chi.NewRouter()
	r.Use(middleware.Recoverer)
	r.Mount("/api/v1", v1.NewRouter(a))

	srv := &http.Server{
		Handler: r,
	}

	log.Info().Msgf("Starting UNIX socket on %s", constant.DefaultSocketPath)
	go func() {
		if e := srv.Serve(socket); e != nil && e != http.ErrServerClosed {
			errChan <- fmt.Errorf("failed to serve UNIX socket: %v", e)
		}
		socket.Close()
		os.Remove(constant.DefaultSocketPath)
	}()

	return srv, nil
}
