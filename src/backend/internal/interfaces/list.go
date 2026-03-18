package interfaces

import (
	"fmt"
	"net"
	"slices"

	"magitrickle/constant"
	"magitrickle/models"

	"github.com/rs/zerolog/log"
)

func List(showAll bool) ([]models.InterfaceInfo, error) {
	networkInterfaces, err := net.Interfaces()
	if err != nil {
		return nil, fmt.Errorf("failed to get interfaces: %w", err)
	}

	if !showAll {
		networkInterfaces = filterManaged(networkInterfaces)
	}

	friendlyNames, err := descriptions()
	if err != nil {
		log.Debug().Err(err).Msg("failed to load interface descriptions")
	}

	interfaces := make([]models.InterfaceInfo, 0, len(networkInterfaces))
	for _, iface := range networkInterfaces {
		interfaces = append(interfaces, models.InterfaceInfo{
			ID:   iface.Name,
			Name: friendlyNames[iface.Name],
		})
	}

	return interfaces, nil
}

func filterManaged(interfaces []net.Interface) []net.Interface {
	filtered := make([]net.Interface, 0, len(interfaces))
	for _, iface := range interfaces {
		if iface.Flags&net.FlagPointToPoint == 0 || slices.Contains(constant.IgnoredInterfaces, iface.Name) {
			continue
		}
		filtered = append(filtered, iface)
	}

	return filtered
}
