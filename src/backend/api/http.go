package api

import (
	"fmt"
	"net"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"

	"magitrickle/api/auth"
	"magitrickle/api/utils"
	v1 "magitrickle/api/v1"
	"magitrickle/app"
	"magitrickle/constant"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/rs/zerolog/log"
)

const (
	noSkinFoundPlaceholder = "<!DOCTYPE html><html><head><title>MagiTrickle</title></head><body><h1>MagiTrickle</h1><p>Please install MagiTrickle skin before using WebUI!</p></body></html>"
	skinsFolderLocation    = constant.AppShareDir + "/skins"
)

func SetupHTTP(a app.Main, errChan chan error) (*http.Server, error) {
	if !a.Config().HTTPWeb.Enabled {
		log.Info().Msg("HTTP WebUI disabled by configuration")
		return nil, nil
	}

	addr := fmt.Sprintf("%s:%d", a.Config().HTTPWeb.Host.Address, a.Config().HTTPWeb.Host.Port)
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("error while listening HTTP %s: %v", addr, err)
	}

	// Создаем основной роутер и монтируем API-роутер, а также статику
	r := chi.NewRouter()
	r.Use(middleware.Recoverer)
	r.Use(func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if !strings.HasPrefix(r.URL.Path, "/api/") {
				next.ServeHTTP(w, r)
				return
			}
			if !a.Config().HTTPWeb.Auth.Enabled || r.URL.Path == "/api/v1/auth" {
				next.ServeHTTP(w, r)
				return
			}
			auth.Middleware(a)(next).ServeHTTP(w, r)
		})
	})
	r.Mount("/api/v1", v1.NewRouter(a))
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
					utils.WriteError(w, http.StatusNotFound, "file not found")
					return
				}
				utils.WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to stat file: %v", err))
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
			utils.WriteError(w, http.StatusInternalServerError, fmt.Sprintf("failed to read file: %v", err))
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
		case ".png":
			w.Header().Set("Content-Type", "image/png")
		default:
			w.Header().Set("Content-Type", "text/plain")
		}
		w.WriteHeader(http.StatusOK)
		w.Write(fileData)
	})

	srv := &http.Server{Handler: r}

	log.Info().Msgf("Starting HTTP server on %s", addr)
	go func() {
		if e := srv.Serve(listener); e != nil && e != http.ErrServerClosed {
			errChan <- fmt.Errorf("failed to serve HTTP: %v", e)
		}
		listener.Close()
	}()
	return srv, nil
}
