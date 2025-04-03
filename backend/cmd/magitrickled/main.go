package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"syscall"

	"github.com/Ponywka/MagiTrickle/backend/constant"
	"github.com/Ponywka/MagiTrickle/backend/pkg/api"
	v1 "github.com/Ponywka/MagiTrickle/backend/pkg/api/v1"
	"github.com/Ponywka/MagiTrickle/backend/pkg/app"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"
)

const (
	noSkinFoundPlaceholder = "<!DOCTYPE html><html><head><title>MagiTrickle</title></head><body><h1>MagiTrickle</h1><p>Please install MagiTrickle skin before using WebUI!</p></body></html>"
	skinsFolderLocation    = constant.AppShareDir + "/skins"
)

func setupUnixSocket(apiRouter chi.Router, errChan chan error) (*http.Server, error) {
	if err := os.Remove(api.SocketPath); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("failed to remove existing UNIX socket: %w", err)
	}

	socket, err := net.Listen("unix", api.SocketPath)
	if err != nil {
		return nil, fmt.Errorf("error while serving UNIX socket: %v", err)
	}

	r := chi.NewRouter()
	r.Use(middleware.Recoverer)
	r.Mount("/api", apiRouter)

	srv := &http.Server{
		Handler: r,
	}

	go func() {
		if e := srv.Serve(socket); e != nil && e != http.ErrServerClosed {
			errChan <- fmt.Errorf("failed to serve UNIX socket: %v", e)
		}
		socket.Close()
		os.Remove(api.SocketPath)
	}()

	return srv, nil
}

func setupHTTP(a *app.App, apiRouter chi.Router, errChan chan error) (*http.Server, error) {
	listener, err := net.Listen("tcp", fmt.Sprintf("%s:%d",
		a.Config().HTTPWeb.Host.Address, a.Config().HTTPWeb.Host.Port))
	if err != nil {
		return nil, fmt.Errorf("error while listening HTTP: %v", err)
	}

	// Создаем основной роутер и монтируем API-роутер, а также статику
	r := chi.NewRouter()
	r.Use(middleware.Recoverer)
	r.Mount("/api", apiRouter)
	r.Get("/*", func(w http.ResponseWriter, r *http.Request) {
		originalFilePath := path.Clean(r.URL.Path)
		filePath := path.Join(skinsFolderLocation, a.Config().HTTPWeb.Skin, originalFilePath)
		// Если запрошен каталог – пытаемся найти index.html
		for i := 0; i < 2; i++ {
			stat, err := os.Stat(filePath)
			if err != nil {
				if os.IsNotExist(err) {
					if originalFilePath == "/" {
						w.WriteHeader(http.StatusNotFound)
						w.Write([]byte(noSkinFoundPlaceholder))
						return
					}
					v1.WriteError(w, http.StatusNotFound, "file not found")
					return
				}
				v1.WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to stat file: %v", err))
				return
			}
			if stat.IsDir() {
				filePath = path.Join(filePath, "index.html")
				continue
			}
			break
		}
		fileData, err := os.ReadFile(filePath)
		if err != nil {
			v1.WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to read file: %v", err))
			return
		}
		ext := filepath.Ext(filePath)
		switch strings.ToLower(ext) {
		case ".html":
			w.Header().Set("Content-Type", "text/html")
		case ".css":
			w.Header().Set("Content-Type", "text/css")
		case ".js":
			w.Header().Set("Content-Type", "application/javascript")
		case ".ico":
			w.Header().Set("Content-Type", "image/x-icon")
		default:
			w.Header().Set("Content-Type", "text/plain")
		}
		w.WriteHeader(http.StatusOK)
		w.Write(fileData)
	})

	srv := &http.Server{
		Handler: r,
	}
	// Запуск HTTP сервера в горутине
	go func() {
		if e := srv.Serve(listener); e != nil && e != http.ErrServerClosed {
			errChan <- fmt.Errorf("failed to serve HTTP: %v", e)
		}
		listener.Close()
	}()
	return srv, nil
}

func main() {
	// Настройка zerolog
	log.Logger = log.Output(zerolog.ConsoleWriter{Out: os.Stderr})
	log.Info().
		Str("version", constant.Version).
		Str("commit", constant.Commit).
		Msg("starting MagiTrickle daemon")

	core := app.New()

	log.Info().Msg("starting service")

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Запуск ядра приложения в горутине
	appResult := make(chan error, 1)
	go func() {
		appResult <- core.Start(ctx)
	}()

	// Создаем API-роутер
	apiHandler := v1.NewHandler(core)
	apiRouter := v1.NewRouter(apiHandler)

	// Запуск HTTP сервера (для WebUI)
	errChan := make(chan error, 1)
	srvHTTP, err := setupHTTP(core, apiRouter, errChan)
	if err != nil {
		log.Fatal().Err(err).Msg("setupHTTP error")
	}

	// Запуск UNIX-сокет сервера
	srvUnix, err := setupUnixSocket(apiRouter, errChan)
	if err != nil {
		log.Fatal().Err(err).Msg("setupUnixSocket error")
	}

	addr := fmt.Sprintf("%s:%d", core.Config().HTTPWeb.Host.Address, core.Config().HTTPWeb.Host.Port)
	log.Info().Msgf("Starting UNIX socket on %s", api.SocketPath)
	log.Info().Msgf("Starting HTTP server on %s", addr)

	// Обработка системных сигналов для graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)

	var once sync.Once
	shutdown := func() {
		log.Info().Msg("shutting down service")
		cancel()
		if err := srvHTTP.Shutdown(context.Background()); err != nil {
			log.Error().Err(err).Msg("HTTP server shutdown error")
		}
		if err := srvUnix.Shutdown(context.Background()); err != nil {
			log.Error().Err(err).Msg("UNIX socket server shutdown error")
		}
	}

	for {
		select {
		case err := <-appResult:
			if err != nil {
				log.Error().Err(err).Msg("failed to start application")
			}
			once.Do(shutdown)
			log.Info().Msg("service stopped")
			return
		case err := <-errChan:
			if err != nil {
				log.Error().Err(err).Msg("server error")
			}
			once.Do(shutdown)
		case sig := <-sigChan:
			log.Info().Msgf("received signal: %v", sig)
			switch sig {
			case os.Interrupt, syscall.SIGTERM:
				once.Do(shutdown)
			case syscall.SIGHUP:
				if err := core.LoadConfig(); err != nil {
					log.Error().Err(err).Msg("failed to reload config")
				}
			}
		}
	}
}
