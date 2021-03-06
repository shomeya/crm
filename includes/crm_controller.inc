<?php
// $Id$

/**
 * @file
 *
 */

class CRMEntityController extends DrupalDefaultEntityController {
  protected $defaultEntities, $cacheComplete = FALSE;
  public $nameKey, $statusKey, $bundleKey, $moduleKey, $entityType;

  /**
   * Overridden.
   * @see DrupalDefaultEntityController#__construct()
   *
   * Allows specifying a name key serving as uniform identifier for this entity
   * type while still internally we are using numeric identifieres.
   */
  public function __construct($entityType, $defaults = array()) {
    parent::__construct($entityType);
    // Use the name key as primary identifier.
    $this->nameKey = isset($this->entityInfo['entity keys']['name']) ? $this->entityInfo['entity keys']['name'] : $this->idKey;
    if (!empty($this->entityInfo['exportable'])) {
      $this->entityInfo['entity keys'] += array('module' => 'module', 'status' => 'status');
      $this->statusKey = $this->entityInfo['entity keys']['status'];
      $this->moduleKey = $this->entityInfo['entity keys']['module'];
    }

    // Load defaults
    foreach ($defaults as $key => $value) {
      $this->{$key} = $value;
    }

    // Initialize known properties
  }

  public function create(array $values = array()) {
    global $user;
    if (isset($this->entityInfo['entity class']) && $class = $this->entityInfo['entity class']) {
      // Add is_new property if it is not set.
      $values += array(
        'is_new' => TRUE,
        'title' => '',
        'uid' => $user->uid,
      );
      return new $class($this->entityType, $values);
    }
    return (object) $values;
  }

  /**
   * Builds and executes the query for loading.
   *
   * @return The results in a Traversable object.
   */
  public function query($ids, $conditions, $revision_id = FALSE) {
    // Build the query.
    $query = $this->buildQuery($ids, $conditions, $revision_id);
    $result = $query->execute();
    if (!empty($this->entityInfo['entity class'])) {
      $result->setFetchMode(PDO::FETCH_CLASS, $this->entityInfo['entity class'], array($this->entityType));
    }
    return $result;
  }

  protected function buildQuery($ids, $conditions = array(), $revision_id = FALSE) {
    // Add the id condition ourself, as we might have a separate name key.
    $query = parent::buildQuery(array(), $conditions, $revision_id);
    if ($ids) {
      // Support loading by numeric ids as well as by machine names.
      $key = is_numeric(reset($ids)) ? $this->idKey : $this->nameKey;
      $query->condition("base.$key", $ids, 'IN');
    }
    return $query;
  }

  /**
   * Overridden.
   * @see DrupalDefaultEntityController#load($ids, $conditions)
   *
   * In contrast to the parent implementation we factor out query execution, so
   * fetching can be further customized easily. Also we add any in code defined
   * objects.
   */
  public function load($ids = array(), $conditions = array()) {
    $entities = array();

    // Revisions are not statically cached, and require a different query to
    // other conditions, so separate the revision id into its own variable.
    if ($this->revisionKey && isset($conditions[$this->revisionKey])) {
      $revision_id = $conditions[$this->revisionKey];
      unset($conditions[$this->revisionKey]);
    }
    else {
      $revision_id = FALSE;
    }

    // Create a new variable which is either a prepared version of the $ids
    // array for later comparison with the entity cache, or FALSE if no $ids
    // were passed. The $ids array is reduced as items are loaded from cache,
    // and we need to know if it's empty for this reason to avoid querying the
    // database when all requested entities are loaded from cache.
    $passed_ids = !empty($ids) ? array_flip($ids) : FALSE;

    // Add default entities defined in code to the cache.
    if (!empty($this->entityInfo['exportable']) && !isset($this->defaultEntities)) {
      $this->getDefaults();
    }
    // Try to load entities from the static cache.
    if (!$revision_id) {
      $entities = $this->cacheGet($ids, $conditions);
      // If any entities were loaded, remove them from the ids still to load.
      if ($passed_ids) {
        $ids = array_keys(array_diff_key($passed_ids, $entities));
      }
    }

    // Load any remaining entities from the database. This is the case if $ids
    // is set to FALSE (so we load all entities), if there are any ids left to
    // load, if loading a revision, or if $conditions was passed without $ids.
    if (!$this->cacheComplete && ($ids === FALSE || $ids || $revision_id || ($conditions && !$passed_ids))) {
      $schema = drupal_get_schema($this->entityInfo['base table']);
      $queried_entities = array();
      foreach ($this->query($ids, $conditions, $revision_id) as $record) {
        // Skip entities already retrieved from cache.
        if (isset($entities[$record->{$this->nameKey}])) {
          continue;
        }

        // Care for serialized columns.
        foreach ($schema['fields'] as $field => $info) {
          if (!empty($info['serialize']) && isset($record->$field)) {
            $record->$field = unserialize($record->$field);
            // Support automatic merging of 'data' fields into the entity.
            if (!empty($info['merge']) && is_array($record->$field)) {
              foreach ($record->$field as $key => $value) {
                $record->$key = $value;
              }
              unset($record->$field);
            }
          }
        }
        if (isset($this->statusKey)) {
          // Care for setting the status key properly.
          $record->{$this->statusKey} |= ENTITY_IN_DB;
          $id = $record->{$this->nameKey};
          if (isset($this->defaultEntities[$id])) {
            $record->{$this->statusKey} |= ENTITY_IN_CODE;
            $record->{$this->moduleKey} = $this->defaultEntities[$id]->{$this->moduleKey};
          }
        }
        $queried_entities[$record->{$this->nameKey}] = $record;
      }
    }

    // Pass all entities loaded from the database through $this->attachLoad(),
    // which attaches fields (if supported by the entity type) and calls the
    // entity type specific load callback, for example hook_node_load().
    if (!empty($queried_entities)) {
      $this->attachLoad($queried_entities, $revision_id);
      $entities += $queried_entities;
    }

    if ($this->cache) {
      // Add entities to the cache if we are not loading a revision.
      if (!empty($queried_entities) && !$revision_id) {
        $this->cacheSet($queried_entities);

        // Remember we have cached all entities now.
        if (!$conditions && $ids === FALSE) {
          $this->cacheComplete = TRUE;
        }
      }
    }
    // Ensure that the returned array is ordered the same as the original
    // $ids array if this was passed in and remove any invalid ids.
    if ($passed_ids && $passed_ids = array_intersect_key($passed_ids, $entities)) {
      foreach ($passed_ids as $id => $value) {
        $passed_ids[$id] = $entities[$id];
      }
      $entities = $passed_ids;
    }
    return $entities;
  }

  /**
   * For exportables, incorporate defaults into the cache.
   */
  protected function getDefaults() {
    $this->defaultEntities = array();
    $hook = isset($this->entityInfo['export']['default hook']) ? $this->entityInfo['export']['default hook'] : 'default_' . $this->entityType;
    // Invoke the hook and collect default entities.
    foreach (module_implements($hook) as $module) {
      foreach ((array) module_invoke($module, $hook) as $id => $entity) {
        $entity->{$this->statusKey} |= ENTITY_IN_CODE;
        $entity->{$this->nameKey} = $id;
        $entity->{$this->moduleKey} = $module;
        $this->defaultEntities[$id] = $entity;
      }
    }
    drupal_alter($hook, $this->defaultEntities);
    // Remove any default entities that have been overridden.
    foreach ($this->query(array_keys($this->defaultEntities), array()) as $record) {
      $overridden[$record->{$this->nameKey}] = TRUE;
    }
    // Handle fetching defaults via the cache.
    $this->cacheSet(isset($overridden) ? array_diff_key($this->defaultEntities, $overridden) : $this->defaultEntities);
  }

  protected function applyConditions($entities, $conditions = array()) {
    if ($conditions) {
      foreach ($entities as $key => $entity) {
        $entity_values = (array) $entity;
        if (array_diff_assoc($conditions, $entity_values)) {
          unset($entities[$key]);
        }
      }
    }
    return $entities;
  }

  /**
   * Overridden.
   * @see includes/DrupalDefaultEntityController#cacheGet($ids, $conditions)
   *
   * If there is nameKey given, we index our entities by this key. This
   * overrides cacheGet() to respect that when applying $conditions.
   */
  protected function cacheGet($ids, $conditions = array()) {
    if (!empty($this->entityCache)) {
      // First get the entities by ids, then apply the conditions.
      $entities = is_array($ids) ? array_intersect_key($this->entityCache, array_flip($ids)) : $this->entityCache;
      return $this->applyConditions($entities, $conditions);
    }
    return array();
  }

  /**
   * Overridden.
   * @see DrupalDefaultEntityController::attachLoad()
   *
   * Fixed to make attaching fields to entities having a name key work.
   */
  protected function attachLoad(&$queried_entities, $revision_id = FALSE) {
    // Attach fields.
    if ($this->entityInfo['fieldable']) {
      $entities = $queried_entities;
      if ($this->nameKey != $this->idKey) {
        // Field API assumes queried entities are keyed by the idkey, thus
        // adapt the array accordingly for it.
        $entities = array();
        foreach ($queried_entities as $entity) {
          $entities[$entity->{$this->idKey}] = $entity;
        }
      }

      if ($revision_id) {
        field_attach_load_revision($this->entityType, $entities);
      }
      else {
        field_attach_load($this->entityType, $entities);
      }
    }

    // Call hook_entity_load().
    foreach (module_implements('entity_load') as $module) {
      $function = $module . '_entity_load';
      $function($queried_entities, $this->entityType);
    }
    // Call hook_TYPE_load(). The first argument for hook_TYPE_load() are
    // always the queried entities, followed by additional arguments set in
    // $this->hookLoadArguments.
    $args = array_merge(array($queried_entities), $this->hookLoadArguments);
    foreach (module_implements($this->entityInfo['load hook']) as $module) {
      call_user_func_array($module . '_' . $this->entityInfo['load hook'], $args);
    }
  }

  public function resetCache(array $ids = NULL) {
    unset($this->defaultEntities);
    $this->cacheComplete = FALSE;
    if (isset($ids)) {
      foreach ($ids as $id) {
        unset($this->entityCache[$id]);
      }
    }
    else {
      $this->entityCache = array();
    }
  }

  /**
   * @param $transaction
   *   Optionally a DatabaseTransaction object to use. Allows overrides to pass
   *   in their transaction object.
   */
  public function save(DatabaseTransaction $transaction = NULL) {
    $transaction = isset($transaction) ? $transaction : db_transaction();

    try {
      if (!isset($this->log)) {
        $this->log = '';
      }
      
      $this->updated = REQUEST_TIME;

      // Always unset the vid, we'll get a new one when we insert into the
      // revision table
      if (isset($this->{$this->revisionKey})) {

        unset($this->{$this->revisionKey});
      }

      // Load the stored entity, if any.
      if (!empty($this->{$this->nameKey}) && !isset($this->original)) {
        $this->original = entity_load_unchanged($this->entityType, $this->{$this->nameKey});
      }

      $this->invoke('presave', $this);

      if (!empty($this->{$this->idKey}) && empty($this->is_new)) {
        // Perform an update
        $op = 'update';
        drupal_write_record($this->entityInfo['base table'], $this, $this->idKey);
        drupal_write_record($this->entityInfo['revision table'], $this);
        $this->resetCache(array($this->{$this->nameKey}));
      }
      else {
        // Save a new entity
        $op = 'insert';
        $this->created = $this->updated;
        drupal_write_record($this->entityInfo['base table'], $this);
        drupal_write_record($this->entityInfo['revision table'], $this);

        if (!empty($this->entityInfo['exportable'])) {
          $this->{$this->statusKey} |= ENTITY_IN_DB;
          $this->resetCache(array($this->{$this->nameKey}));
          if (!entity_has_status($this->entityType, $this, ENTITY_IN_CODE)) {
            $entities = array($this->{$this->nameKey} => $this);
            module_invoke_all($this->entityType . '_enabled', $entities);
            module_invoke_all('entity_enabled', $entities, $this->entityType);
          }
        }
      }
      // Since we always save a new revision, update the vid
      db_update($this->entityInfo['base table'])
        ->fields(array($this->revisionKey => $this->{$this->revisionKey}))
        ->condition($this->idKey, $this->{$this->idKey})
        ->execute();

      $this->invoke($op, $this);
      // Ignore slave server temporarily.
      db_ignore_slave();
      unset($this->is_new);
      unset($this->original);

      return $this;
    }
    catch (Exception $e) {
      $transaction->rollback();
      watchdog_exception($this->entityType, $e);
      throw $e;
    }
  }
  
  public function invoke($hook) {
    if (!empty($this->entityInfo['fieldable']) && function_exists($function = 'field_attach_' . $hook)) {
      $function($this->entityType, $this);
    }
    
    // Invoke the hook. If rules is there, use the rule function so that a rules
    // event is invoked too.
    if (module_exists('rules')) {
      rules_invoke_all($this->entityType . '_' . $hook, $this);
    }
    else {
      module_invoke_all($this->entityType . '_' . $hook, $this);
    }
    // Invoke the respective entity level hook.
    if ($hook == 'presave' || $hook == 'insert' || $hook == 'update' || $hook == 'delete') {
      module_invoke_all('entity_' . $hook, $this, $this->entityType);
    }
  }

  public function delete(DatabaseTransaction $transaction = NULL) {
    return $this->deleteMultiple(array($this->{$this->idKey}), $transaction);
  }

  /**
   *
   *
   * @param $transaction
   *   Optionally a DatabaseTransaction object to use. Allows overrides to pass
   *   in their transaction object.
   */
  public function deleteMultiple($ids, DatabaseTransaction $transaction = NULL) {
    if (!empty($ids)) {
      $entities = $this->load($ids);
      $transaction = isset($transaction) ? $transaction : db_transaction();

      // Ensure the contacts can actually be deleted.
      foreach ((array) $entities as $id => $entity) {
        if (in_array(FALSE, module_invoke_all($this->entityType . '_can_delete', $entity))) {
          unset($entities[$id]);
        }
      }

      // If none of the specified contacts can be deleted, return FALSE.
      if (empty($entities)) {
        return FALSE;
      }

      try {
        db_delete($this->entityInfo['base table'])
          ->condition($this->nameKey, $ids, 'IN')
          ->execute();
        db_delete($this->entityInfo['revision table'])
          ->condition($this->nameKey, $ids, 'IN')
          ->execute();
        // Reset the cache as soon as the changes have been applied.
        $this->resetCache($ids);

        foreach ($entities as $id => $entity) {
          $this->invoke('delete', $entity);
          if (!empty($this->entityInfo['exportable']) && !($entity->{$this->statusKey} & ENTITY_IN_CODE)) {
            $disabled_entities[$id] = $entity;
          }
        }
        if (!empty($disabled_entities)) {
          module_invoke_all($this->entityType . '_disabled', $disabled_entities);
          module_invoke_all('entity_disabled', $disabled_entities, $this->entityType);
        }
        // Ignore slave server temporarily.
        db_ignore_slave();
      }
      catch (Exception $e) {
        $transaction->rollback();
        watchdog_exception($this->entityType, $e);
        throw $e;
      }
      return TRUE;
    }
  }

  /**
   *
   *
   * @return
   *   A serialized string in JSON format suitable for the import() method.
   */
  public function export($entity, $prefix = '') {
    $vars = get_object_vars($entity);
    unset($vars[$this->statusKey], $vars['is_new']);
    if ($this->nameKey != $this->idKey) {
      unset($vars[$this->idKey]);
    }
    return entity_var_json_export($vars, $prefix);
  }

  /**
   *
   *
   * @param $export
   *   A serialized string in JSON format as produced by the export() method.
   */
  public function import($export) {
    $vars = drupal_json_decode($export);
    if (is_array($vars)) {
      return $this->create($vars);
    }
    return FALSE;
  }

  /**
   * 
   *
   * @param $content
   *   Optionally. Allows pre-populating the built content to ease overridding
   *   this method.
   */
  public function buildContent($entity, $view_mode = 'full', $langcode = NULL, $content = array()) {
    // Remove previously built content, if exists.
    $entity->content = $content;
    $langcode = isset($langcode) ? $langcode : $GLOBALS['language_content']->language;

    // Add in fields.
    if (!empty($this->entityInfo['fieldable'])) {
      $entity->content += field_attach_view($this->entityType, $entity, $view_mode, $langcode);
    }
    // Invoke hook_ENTITY_view() to allow modules to add their additions.
    if (module_exists('rules')) {
      rules_invoke_all($this->entityType . '_view', $entity, $view_mode, $langcode);
    }
    else {
      module_invoke_all($this->entityType . '_view', $entity, $view_mode, $langcode);
    }
    module_invoke_all('entity_view', $entity, $this->entityType, $view_mode, $langcode);
    $build = $entity->content;
    unset($entity->content);
    return $build;
  }

  /**
   *
   */
  public function view($entities, $view_mode = 'full', $langcode = NULL) {
    if (!empty($this->entityInfo['fieldable'])) {
      field_attach_prepare_view($this->entityType, $entities, $view_mode);
    }
    entity_prepare_view($this->entityType, $entities);
    $langcode = isset($langcode) ? $langcode : $GLOBALS['language_content']->language;

    $view = array();
    foreach ($entities as $key => $entity) {
      $build = entity_build_content($this->entityType, $entity, $view_mode, $langcode);
      $build += array(
        // If the entity type provides an implementation, use this instead the
        // generic one.
        '#theme' => 'entity',
        '#entity_type' => $this->entityType,
        '#entity' => $entity,
        '#view_mode' => $view_mode,
        '#language' => $langcode,
      );
      // Allow modules to modify the structured entity.
      drupal_alter(array($this->entityType . '_view', 'entity_view'), $build, $this->entityType);
      $view[$this->entityType][$key] = $build;
    }
    return $view;
  }
}
