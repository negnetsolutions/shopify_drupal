<?php
/**
 * @file
 * Contains functions relating to Shopify webhooks.
 */

/**
 * Shopify webhook menu callback.
 */
function shopify_incoming_webhook() {
  $json = file_get_contents('php://input');
  $hook = str_replace('/', '_', $_SERVER['HTTP_X_SHOPIFY_TOPIC']);

  // Log webhooks if the debug option is set.
  if (variable_get('shopify_webhooks_debug', FALSE)) {
    watchdog('shopify_webhook', $hook . ':' . check_plain(var_export(json_decode($json,true), TRUE)));
  }

  //add the webhook to the queue
  shopify_shopify_webhook_queue($hook,$json);
}


function shopify_shopify_webhook_findUnprocessedQueueBefore($product_id, $time=false){
  $time = ($time === false) ? time() : $time;

  $regex = '"id":'.$product_id;

  $query = <<<EOL
select *
from shopify_webhook_queue q
where processed = 0
and q.payload REGEXP '$regex'
and q.created_at < $time
order by q.created_at ASC
EOL;

  $result = db_query($query);
  $ids = array();
  foreach($result as $record){
    $ids[] = $record->id;
  }

  return $ids;
}

function shopify_shopify_webhook_processqueue($limit=false){

  $limit = ($limit === false) ? variable_get('shopify_webhooks_que_process_limit', 10) : $limit;

  //get queues to process -> only $limit per run
  $query = <<<EOL
select *
from shopify_webhook_queue q
where processed = 0
order by q.created_at ASC
limit $limit
EOL;

  $result = db_query($query);
  $ids = array();
  foreach($result as $record){
    $hook = $record->command;
    $payload = json_decode($record->payload, true);
    $id = $record->id;

    // Allow modules to hook into webhooks firing.
    module_invoke_all('shopify_webhook', $hook, $payload);
    module_invoke_all('shopify_webhook_' . $hook, $payload);

    shopify_shopify_webhook_markQueueProcessed(array($id), 1);
  }

  //delete old processed hooks
  $deleted = db_delete('shopify_webhook_queue')
    ->condition('processed',1)
    ->condition('created_at', (time() - 1209600), '<')
    ->execute()
  ;
  $deleted = db_delete('shopify_webhook_queue')
    ->condition('processed',2)
    ->condition('created_at', (time() - 1209600), '<')
    ->execute()
  ;
}

function shopify_shopify_webhook_markQueueProcessed($ids, $status=1){

  foreach($ids as $id){
    //set the webhook to processed
    $updated = db_update('shopify_webhook_queue')
      ->fields(array(
        'processed' => $status
      ))
      ->condition('id',$id)
      ->execute()
    ;
  }

  return true;
}

function shopify_shopify_webhook_queue($hook,$json){

  $id = db_insert('shopify_webhook_queue')->
    fields(array(
      'command'=>$hook,
      'payload'=>$json,
      'received_ip'=>ip2long($_SERVER['REMOTE_ADDR']),
      'created_at'=>time(),
      'processed'=>0
    ))
    ->execute();
  return $id;
}

/**
 * Implements hook_shopify_webhook().
 */
function shopify_shopify_webhook($hook, $payload) {
  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_fired', $hook);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_products_create($product) {
  shopify_shopify_webhook_products_update($product);
  if (module_exists('rules')) {
    $product_entity = shopify_product_load_single($product['id']);
    rules_invoke_event('shopify_webhook_product_created', $product_entity);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_collections_update($collection) {
  shopify_collection_update($collection, $sync_products = TRUE);
  if (module_exists('rules')) {
    $term = shopify_collection_load($collection['id']);
    rules_invoke_event('shopify_webhook_collection_updated', $term);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_collections_delete($collection) {
  $term = shopify_collection_load($collection['id']);
  shopify_collection_delete($collection['id']);
  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_collection_deleted', $term);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_orders_paid($order) {
  // Update product inventory counts for purchased products.
  foreach ($order['line_items'] as $line_item) {
    if ($variant = shopify_product_load_by_variant_id($line_item['variant_id'])) {
      shopify_product_update($line_item['product_id'], $variant->variant_id, shopify_api_get_variant($variant->variant_id, array(
        'fields' => 'inventory_quantity,old_inventory_quantity',
      )));
    }
  }
  if (module_exists('rules')) {
    rules_invoke_event_by_args('shopify_webhook_order_paid', shopify_rules_parse_order_args($order));
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_shop_update($payload) {
  // Update store info on the site.
  variable_set('shopify_store_info', $payload);
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_collections_create($collection) {
  shopify_collection_create($collection, $sync_products = TRUE);
  if (module_exists('rules')) {
    $term = shopify_collection_load($collection['id']);
    rules_invoke_event('shopify_webhook_collection_created', $term);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_products_update($product) {
  $product_entity = shopify_product_update($product['id'], 0, $product);
  shopify_sync_alterProductFieldsFromVariant($product, $product_entity);
  $product_entity->save();

  foreach ($product['variants'] as $v) {
    if($v['id'] != 0){
      $variant = shopify_product_update($product['id'], $v['id'], $v);
      $variant->save();
    }
  }

  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_product_updated', $product_entity);
  }
}

/**
 * Implements hook_shopify_webhook_WEBHOOK().
 */
function shopify_shopify_webhook_products_delete($product) {
  $product_entity = shopify_product_load_single($product['id']);
  shopify_delete_by_product_id($product['id']);
  if (module_exists('rules')) {
    rules_invoke_event('shopify_webhook_product_deleted', $product_entity);
  }
}
