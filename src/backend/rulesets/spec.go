package rulesets

import (
	"magitrickle/models"
	"magitrickle/utils/intID"
)

type Spec struct {
	Model      *models.Group
	ID         intID.ID
	RuntimeKey string
	Name       string
	Interface  string
	Enable     bool
	Rules      []*models.Rule
}
